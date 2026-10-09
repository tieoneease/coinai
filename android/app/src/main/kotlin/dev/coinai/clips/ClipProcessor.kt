package dev.coinai.clips

import dev.coinai.protocol.ReplyState
import dev.coinai.sambot.PromptNotSentException
import dev.coinai.sambot.PromptOutcomeUnknownException
import dev.coinai.sambot.SambotApi
import dev.coinai.sambot.SambotException
import java.io.File
import java.io.IOException
import kotlin.time.Duration
import kotlin.time.Duration.Companion.minutes

/** A progress or result line for the coin's screen, sent as `REPLY` if the coin is still connected. */
data class CoinReply(val device: String, val clipId: Int, val state: ReplyState, val text: String)

/** Where processing reports to the coin. The live BLE session forwards these within the reply window. */
fun interface ReplySink {
    fun offer(reply: CoinReply)
}

/** Posts the phone notification for a finished reply. */
fun interface ReplyNotifier {
    fun replyReady(clip: ClipRecord, reply: String)
}

data class ProcessingSettings(
    /** Clips with less voiced time than this are NO_SPEECH and never transcribed or prompted. */
    val voicedThresholdMs: Long = DEFAULT_VOICED_THRESHOLD_MS,
    val lane: String = "web",
    val askTimeout: Duration = 5.minutes,
    /** Attempts (WorkManager runs) before a retryable failure becomes FAILED. */
    val maxAttempts: Int = 8,
) {
    companion object {
        const val DEFAULT_VOICED_THRESHOLD_MS = 400L
    }
}

/**
 * Takes a RECEIVED clip through transcription and the SamBot prompt (PLAN.md "Processing" and
 * "Duplicates"): transcription may be retried freely; the prompt goes out at most once, guarded by
 * [ProcessingStore.claimPrompt]; an unknown prompt outcome makes the clip UNCERTAIN.
 */
class ClipProcessor(
    private val store: ProcessingStore,
    private val sambot: suspend () -> SambotApi,
    private val settings: suspend () -> ProcessingSettings,
    private val notifier: ReplyNotifier,
    private val replies: ReplySink,
    private val readAudio: (String) -> ByteArray = { File(it).readBytes() },
) {
    /** Clips this processor is prompting right now. Process memory, so empty after a crash. */
    private val prompting: MutableSet<Long> = java.util.concurrent.ConcurrentHashMap.newKeySet()

    sealed interface Outcome {
        data object Done : Outcome
        data class Retry(val reason: String) : Outcome
    }

    /** One processing pass. [attempt] counts earlier runs, as WorkManager's `runAttemptCount`. */
    suspend fun process(id: Long, attempt: Int = 0): Outcome {
        val clip = store.get(id) ?: return Outcome.Done
        val config = settings()
        return when (clip.state) {
            ClipState.RECEIVED -> transcribeThenPrompt(clip, config, attempt)
            ClipState.TRANSCRIBED -> prompt(clip, config, attempt)
            ClipState.PROMPTED -> {
                // Claimed but no outcome recorded. If no run in this process holds the claim, the
                // run that did died: whether SamBot got the prompt is unknown, so never resend it
                // automatically. The conditional transition leaves a reply saved meanwhile alone.
                if (id !in prompting) store.transition(id, ClipState.PROMPTED, ClipState.UNCERTAIN, "interrupted while prompting")
                Outcome.Done
            }
            else -> Outcome.Done
        }
    }

    /** Manual resend from UNCERTAIN, NO_SPEECH or FAILED; the caller then schedules [process]. */
    suspend fun resend(id: Long): Boolean = store.prepareResend(id)

    private suspend fun transcribeThenPrompt(clip: ClipRecord, config: ProcessingSettings, attempt: Int): Outcome {
        val voiced = clip.voicedMs ?: 0
        if (!clip.forceSpeech && voiced < config.voicedThresholdMs) {
            store.setState(clip.id, ClipState.NO_SPEECH, "voiced $voiced ms, below ${config.voicedThresholdMs} ms")
            replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.FAILED, NO_SPEECH_TEXT))
            return Outcome.Done
        }
        val path = clip.filePath ?: run {
            store.setState(clip.id, ClipState.FAILED, "clip file missing")
            return Outcome.Done
        }
        replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.TRANSCRIBING, ""))
        val transcript = try {
            sambot().transcribe(readAudio(path))
        } catch (e: IOException) {
            return retryOrFail(clip, config, attempt, "transcribe: ${e.message}", permanent = e.isPermanent())
        }
        if (transcript.isBlank()) {
            store.setState(clip.id, ClipState.NO_SPEECH, "blank transcript")
            replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.FAILED, NO_SPEECH_TEXT))
            return Outcome.Done
        }
        store.saveTranscript(clip.id, transcript.trim())
        return prompt(store.get(clip.id) ?: return Outcome.Done, config, attempt)
    }

    private suspend fun prompt(clip: ClipRecord, config: ProcessingSettings, attempt: Int): Outcome {
        val text = clip.transcript?.takeIf { it.isNotBlank() } ?: run {
            store.setState(clip.id, ClipState.FAILED, "no transcript to prompt")
            return Outcome.Done
        }
        val api = try {
            sambot()
        } catch (e: IllegalArgumentException) {
            store.setState(clip.id, ClipState.FAILED, "SamBot settings: ${e.message}")
            return Outcome.Done
        }
        // The claim commits before anything is sent: a crash from here on leaves PROMPTED, which
        // the next run turns into UNCERTAIN instead of prompting again.
        if (!prompting.add(clip.id)) return Outcome.Done
        try {
            if (!store.claimPrompt(clip.id)) return Outcome.Done
            return promptClaimed(clip, api, text, config, attempt)
        } finally {
            prompting.remove(clip.id)
        }
    }

    private suspend fun promptClaimed(clip: ClipRecord, api: SambotApi, text: String, config: ProcessingSettings, attempt: Int): Outcome {
        replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.THINKING, ""))
        val result = try {
            api.ask(text, lane = config.lane, timeout = config.askTimeout)
        } catch (e: PromptNotSentException) {
            store.releasePrompt(clip.id)
            return retryOrFail(clip, config, attempt, "prompt not sent: ${e.message}", permanent = e.isPermanent())
        } catch (e: PromptOutcomeUnknownException) {
            store.setState(clip.id, ClipState.UNCERTAIN, e.message)
            replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.FAILED, UNCERTAIN_TEXT))
            return Outcome.Done
        } catch (e: SambotException) {
            // SamBot answered and refused (for example 409, agent not running): not delivered.
            store.setState(clip.id, ClipState.FAILED, e.message)
            replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.FAILED, "SamBot: ${e.message}"))
            return Outcome.Done
        } catch (e: IOException) {
            store.setState(clip.id, ClipState.UNCERTAIN, e.message)
            replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.FAILED, UNCERTAIN_TEXT))
            return Outcome.Done
        }
        store.saveReply(clip.id, result.text, result.partial)
        val replied = store.get(clip.id) ?: clip
        notifier.replyReady(replied, result.text)
        replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.DONE, result.text))
        return Outcome.Done
    }

    private suspend fun retryOrFail(clip: ClipRecord, config: ProcessingSettings, attempt: Int, reason: String, permanent: Boolean): Outcome {
        if (permanent || attempt + 1 >= config.maxAttempts) {
            store.setState(clip.id, ClipState.FAILED, reason)
            replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.FAILED, "Failed: $reason"))
            return Outcome.Done
        }
        return Outcome.Retry(reason)
    }

    /** HTTP answers that a retry will not change: client errors other than timeout and rate limit. */
    private fun IOException.isPermanent(): Boolean {
        val status = (this as? SambotException)?.status ?: return false
        return status in 300..499 && status != 408 && status != 429
    }

    companion object {
        const val NO_SPEECH_TEXT = "No speech heard"
        const val UNCERTAIN_TEXT = "Not sure SamBot got it. Resend from the phone."
    }
}
