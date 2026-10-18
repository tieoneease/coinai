package dev.coinai.clips

import dev.coinai.protocol.ReplyState
import dev.coinai.sambot.PromptNotSentException
import dev.coinai.sambot.PromptOutcomeUnknownException
import dev.coinai.sambot.SambotApi
import dev.coinai.sambot.SambotException
import java.io.File
import dev.coinai.sambot.SceneResult
import java.io.IOException
import java.util.TimeZone
import kotlin.time.Duration
import kotlin.time.Duration.Companion.minutes

/**
 * A progress or result line for the coin's screen, sent as `REPLY` if the coin is still connected.
 * With [scene] (compact JSON) it is the clip's final answer as a scene, sent as `SCENE`; [text] is
 * then the card's summary, which the coin gets as a `REPLY` `done` if it cannot show the scene.
 */
data class CoinReply(val device: String, val clipId: Int, val state: ReplyState, val text: String, val scene: String? = null)

/** Where processing reports to the coin. The live BLE session forwards these within the reply window. */
fun interface ReplySink {
    fun offer(reply: CoinReply)
}

/** Posts the phone notification for a finished reply. */
fun interface ReplyNotifier {
    fun replyReady(clip: ClipRecord, reply: String)

    /**
     * A hand-off (a `later` scene's prompt) ended without SamBot's answer. The coin already shows
     * "SamBot is on it" and hears nothing more, so the phone has to say it.
     */
    fun handoffFailed(clip: ClipRecord, reason: String) {}
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
 *
 * For a coin that shows scenes ([showsScenes], `STATUS` bit 3) the transcript goes to the composer
 * first (PROTOCOL.md "Scenes"): its scene is the coin's answer, and a `later` scene's hand-off is
 * prompted instead of the transcript, with nothing more to the coin. When the composer cannot be
 * reached the clip goes the plain way: the transcript is prompted and the reply sent as `REPLY`.
 */
class ClipProcessor(
    private val store: ProcessingStore,
    private val sambot: suspend () -> SambotApi,
    private val settings: suspend () -> ProcessingSettings,
    private val notifier: ReplyNotifier,
    private val replies: ReplySink,
    private val readAudio: (String) -> ByteArray = { File(it).readBytes() },
    /** Whether the coin [device] last reported `STATUS` bit 3 (shows scenes). */
    private val showsScenes: suspend (device: String) -> Boolean = { false },
    /** The phone's time zone, for the composer's `utc_offset_min`. */
    private val timeZone: () -> TimeZone = TimeZone::getDefault,
    private val clock: () -> Long = System::currentTimeMillis,
    private val log: (String) -> Unit = {},
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
                if (id !in prompting && store.transition(id, ClipState.PROMPTED, ClipState.UNCERTAIN, "interrupted while prompting")) {
                    if (clip.handoff != null) notifier.handoffFailed(clip, UNCERTAIN_TEXT)
                }
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
        val api = client(clip) ?: return Outcome.Done
        replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.TRANSCRIBING, ""))
        val transcript = try {
            api.transcribe(readAudio(path))
        } catch (e: IOException) {
            val reason = e.message.orEmpty().let { if (it.startsWith("transcribe")) it else "transcribe: $it" }
            return retryOrFail(clip, config, attempt, reason, e)
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
        val transcript = clip.transcript?.takeIf { it.isNotBlank() } ?: run {
            store.setState(clip.id, ClipState.FAILED, "no transcript to prompt")
            return Outcome.Done
        }
        val api = client(clip) ?: return Outcome.Done
        if (!prompting.add(clip.id)) return Outcome.Done
        try {
            var current = clip
            var thinkingShown = false
            // A clip with a hand-off saved has had its scene: only the hand-off is left to send.
            if (current.handoff == null && showsScenes(current.device)) {
                // PROTOCOL.md "Scenes": `thinking` with the transcript while the scene is composed.
                replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.THINKING, transcript))
                thinkingShown = true
                val composed = compose(current, api, transcript)
                if (composed != null) {
                    val summary = SceneSummary.of(composed.scene)
                    val answer = CoinReply(clip.device, clip.clipId, ReplyState.DONE, summary, scene = composed.scene)
                    // Set exactly for a `later` card; a blank one still goes to SamBot, as the owner's words.
                    val handoff = composed.handoff?.ifBlank { transcript }
                    if (handoff == null) {
                        // Answered by the composer alone: no prompt, no notification.
                        if (store.saveSceneReply(clip.id, composed.scene, summary)) replies.offer(answer)
                        return Outcome.Done
                    }
                    if (!store.saveHandoff(clip.id, composed.scene, handoff)) return Outcome.Done
                    replies.offer(answer)
                    current = store.get(clip.id) ?: return Outcome.Done
                }
            }
            // The claim commits before anything is sent: a crash from here on leaves PROMPTED, which
            // the next run turns into UNCERTAIN instead of prompting again.
            if (!store.claimPrompt(clip.id)) return Outcome.Done
            return promptClaimed(current, api, current.handoff ?: transcript, config, attempt, thinkingShown)
        } finally {
            prompting.remove(clip.id)
        }
    }

    /** `POST /coin/scene`, or null (logged) when the composer cannot be reached or answers badly. */
    private suspend fun compose(clip: ClipRecord, api: SambotApi, transcript: String): SceneResult? {
        val at = clip.startedAtMs
        val offsetMin = timeZone().getOffset(at ?: clock()) / 60_000
        return try {
            api.scene(transcript, at, offsetMin).also {
                if (it.issues.isNotEmpty()) log("clip row ${clip.id}: composer issues: ${it.issues.joinToString("; ")}")
            }
        } catch (e: IOException) {
            log("clip row ${clip.id}: no scene (${e.message}); prompting SamBot with the transcript")
            null
        }
    }

    /**
     * Sends [text] (the transcript, or the hand-off) after the claim. A hand-off ([ClipRecord.handoff])
     * sends nothing to the coin, which already shows "SamBot is on it"; its failures notify instead.
     */
    private suspend fun promptClaimed(
        clip: ClipRecord, api: SambotApi, text: String, config: ProcessingSettings, attempt: Int, thinkingShown: Boolean,
    ): Outcome {
        val toCoin = clip.handoff == null
        // PROTOCOL.md rule 7: `thinking` may carry what SamBot heard; the coin shows it while it waits.
        if (toCoin && !thinkingShown) replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.THINKING, text))
        val result = try {
            api.ask(text, lane = config.lane, timeout = config.askTimeout)
        } catch (e: PromptNotSentException) {
            store.releasePrompt(clip.id)
            return retryOrFail(clip, config, attempt, "prompt not sent: ${e.message}", e)
        } catch (e: PromptOutcomeUnknownException) {
            store.setState(clip.id, ClipState.UNCERTAIN, e.message)
            report(clip, UNCERTAIN_TEXT)
            return Outcome.Done
        } catch (e: SambotException) {
            // SamBot answered and refused (for example 409, agent not running): not delivered.
            store.setState(clip.id, ClipState.FAILED, e.message)
            report(clip, "SamBot: ${e.message}")
            return Outcome.Done
        } catch (e: IOException) {
            store.setState(clip.id, ClipState.UNCERTAIN, e.message)
            report(clip, UNCERTAIN_TEXT)
            return Outcome.Done
        }
        store.saveReply(clip.id, result.text, result.partial)
        val replied = store.get(clip.id) ?: clip
        notifier.replyReady(replied, result.text)
        if (toCoin) replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.DONE, result.text))
        return Outcome.Done
    }

    /** A failure the owner must see: on the coin, or as a notification for a hand-off. */
    private fun report(clip: ClipRecord, text: String) {
        if (clip.handoff != null) notifier.handoffFailed(clip, text)
        else replies.offer(CoinReply(clip.device, clip.clipId, ReplyState.FAILED, text))
    }

    /** The SamBot client, or null after failing the clip when the app's SamBot settings are unusable. */
    private suspend fun client(clip: ClipRecord): SambotApi? = try {
        sambot()
    } catch (e: IllegalArgumentException) {
        store.setState(clip.id, ClipState.FAILED, "SamBot settings: ${e.message}")
        report(clip, SETUP_TEXT)
        null
    }

    private suspend fun retryOrFail(clip: ClipRecord, config: ProcessingSettings, attempt: Int, reason: String, e: IOException): Outcome {
        if (e.isPermanent() || attempt + 1 >= config.maxAttempts) {
            store.setState(clip.id, ClipState.FAILED, reason)
            report(clip, if (e.isAuthFailure()) TOKEN_TEXT else "Failed: $reason")
            return Outcome.Done
        }
        return Outcome.Retry(reason)
    }

    /** The exe.dev proxy refused the token: 401/403, or a redirect to its login page. */
    private fun IOException.isAuthFailure(): Boolean {
        val status = (this as? SambotException)?.status ?: return false
        return status == 401 || status == 403 || status in 300..399
    }

    /** HTTP answers that a retry will not change: client errors other than timeout and rate limit. */
    private fun IOException.isPermanent(): Boolean {
        val status = (this as? SambotException)?.status ?: return false
        return status in 300..499 && status != 408 && status != 429
    }

    companion object {
        const val NO_SPEECH_TEXT = "No speech heard"
        const val UNCERTAIN_TEXT = "Not sure SamBot got it. Resend from the phone."
        const val SETUP_TEXT = "Set up SamBot in the coinai app"
        const val TOKEN_TEXT = "SamBot didn't accept the app's token"
    }
}
