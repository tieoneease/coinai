package dev.coinai.clips

/**
 * A clip's lifecycle on the phone:
 * RECEIVING → RECEIVED → TRANSCRIBED → PROMPTED → REPLIED, with the side exits NO_SPEECH (too
 * little voice or a blank transcript; never prompted), UNCERTAIN (the prompt may or may not have
 * reached SamBot; never retried automatically) and FAILED.
 */
enum class ClipState {
    RECEIVING, RECEIVED, TRANSCRIBED, PROMPTED, REPLIED, NO_SPEECH, UNCERTAIN, FAILED;

    /** States the user may resend from. */
    val resendable: Boolean get() = this == UNCERTAIN || this == NO_SPEECH || this == FAILED
}

data class ClipRecord(
    val id: Long,
    val device: String,
    val clipId: Int,
    val state: ClipState,
    val size: Long? = null,
    val crc32: Long? = null,
    val durationMs: Long? = null,
    val voicedMs: Long? = null,
    /** Synced bytes of the part file while RECEIVING; the resume offset. */
    val receivedBytes: Long = 0,
    /** Estimated recording start (PROTOCOL.md rule 4). */
    val startedAtMs: Long? = null,
    val receivedAtMs: Long? = null,
    val ackedAtMs: Long? = null,
    /** The verified `.ogg`, once RECEIVED. */
    val filePath: String? = null,
    val transcript: String? = null,
    val reply: String? = null,
    val replyPartial: Boolean = false,
    /** The composer's scene (PROTOCOL.md "Scenes"), compact JSON, when the coin was answered with one. */
    val scene: String? = null,
    /** The text prompted to SamBot for a `later` scene, instead of the transcript. */
    val handoff: String? = null,
    val error: String? = null,
    /** Set by a manual resend of a NO_SPEECH clip: skip the voiced-time gate once. */
    val forceSpeech: Boolean = false,
    val createdAtMs: Long = 0,
)

/** What clip processing needs from storage. Room implements it; tests use a map. */
interface ProcessingStore {
    suspend fun get(id: Long): ClipRecord?

    /** RECEIVED → TRANSCRIBED with the transcript. */
    suspend fun saveTranscript(id: Long, transcript: String)

    /**
     * TRANSCRIBED → PROMPTED in one transaction, before the prompt is sent. True only for the
     * caller that made the transition, so each clip is prompted at most once.
     */
    suspend fun claimPrompt(id: Long): Boolean

    /** PROMPTED → TRANSCRIBED, only when the prompt certainly did not reach SamBot. */
    suspend fun releasePrompt(id: Long)

    suspend fun saveReply(id: Long, reply: String, partial: Boolean)

    /**
     * TRANSCRIBED → REPLIED for a scene the composer answered alone: the scene, and its summary as
     * the reply. False when the clip was not TRANSCRIBED.
     */
    suspend fun saveSceneReply(id: Long, scene: String, summary: String): Boolean

    /**
     * Keeps a `later` scene and its hand-off text on a TRANSCRIBED clip, which stays TRANSCRIBED
     * until the hand-off is claimed like any prompt. False when the clip was not TRANSCRIBED.
     */
    suspend fun saveHandoff(id: Long, scene: String, handoff: String): Boolean

    suspend fun setState(id: Long, state: ClipState, error: String? = null)

    /** [from] → [to] only if the clip is still in [from]; true when it changed. */
    suspend fun transition(id: Long, from: ClipState, to: ClipState, error: String? = null): Boolean

    /**
     * Manual resend of an UNCERTAIN, NO_SPEECH or FAILED clip that has its file: back to
     * TRANSCRIBED (or RECEIVED without a transcript). False when the clip is not resendable.
     */
    suspend fun prepareResend(id: Long): Boolean
}
