package dev.coinai.sambot

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.channels.SendChannel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import okhttp3.Call
import okhttp3.Callback
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.MultipartBody
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import java.io.IOException
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.time.Duration
import kotlin.time.Duration.Companion.minutes

/** A failed SamBot call. [status] is the HTTP status when the server answered. */
open class SambotException(message: String, val status: Int? = null, cause: Throwable? = null) : IOException(message, cause)

/**
 * `POST /prompt` failed in a way that leaves its outcome unknown (a network error or timeout after
 * the request may have reached the server). PLAN.md "Duplicates": such a clip is marked uncertain
 * and offers a manual resend instead of an automatic retry.
 */
class PromptOutcomeUnknownException(message: String, cause: Throwable? = null) : SambotException(message, null, cause)

/** [partial] is true when the reply ended by timeout or stream failure instead of `agent_settled`. */
data class AskResult(val text: String, val steered: Boolean, val partial: Boolean)

/**
 * Client for the user's SamBot server through the exe.dev proxy (PLAN.md D13, D14). Mirrors the
 * server's reference client `~/.sambot/telegram/src/sambot-client.ts`.
 *
 * Every request carries `X-Exedev-Authorization: Bearer <token>`, which the proxy consumes.
 * Redirects are never followed: the proxy redirects an unauthenticated request to its login page,
 * and that surfaces here as a [SambotException] instead of a confusing HTML 200.
 */
class SambotClient(baseUrl: String, private val token: String, okHttpClient: OkHttpClient) {
    private val base: HttpUrl = requireNotNull(baseUrl.trim().toHttpUrlOrNull()) { "not an http(s) URL: $baseUrl" }
    private val http: OkHttpClient = okHttpClient.newBuilder().followRedirects(false).followSslRedirects(false).build()
    /** The event stream stays open for a whole turn; [ask]'s own timeout bounds it instead. */
    private val streamHttp: OkHttpClient = http.newBuilder()
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .callTimeout(0, TimeUnit.MILLISECONDS)
        .build()

    /** `GET /health`; true when the server answers `{"ok": true}`. */
    suspend fun health(): Boolean = withContext(Dispatchers.IO) {
        http.newCall(request(url("health")).get().build()).await().use { response ->
            if (!response.isSuccessful) throw response.failure("GET /health")
            response.decode<HealthResponse>("GET /health").ok
        }
    }

    /** `POST /voice/transcribe` with the clip unchanged; returns the transcript. */
    suspend fun transcribe(ogg: ByteArray): String = withContext(Dispatchers.IO) {
        val body = MultipartBody.Builder()
            .setType(MultipartBody.FORM)
            .addFormDataPart("audio", "clip.ogg", ogg.toRequestBody(OGG))
            .build()
        http.newCall(request(url("voice/transcribe")).post(body).build()).await().use { response ->
            if (!response.isSuccessful) throw response.failure("transcribe")
            response.decode<TranscribeResponse>("transcribe").text
        }
    }

    /**
     * Sends [text] to the [lane] conversation and collects the assistant's reply:
     * 1. open `GET /events?lane=` and discard everything up to the first `host_stream_live` (the
     *    replay/live boundary; replay holds old `message_end` and `agent_settled` frames);
     * 2. only then `POST /prompt`;
     * 3. append assistant text from `message_end`/`message` events until the first `agent_settled`.
     *
     * Frames are handled one at a time, so frames after `agent_settled` in the same network chunk
     * (a later turn) are never appended. When [timeout] passes or the stream ends after the prompt
     * was accepted, the text so far comes back with `partial = true`. Failing before the prompt
     * was sent throws [SambotException]; a prompt with an unknown outcome throws
     * [PromptOutcomeUnknownException]. [onDelta] runs in the caller's context with each chunk.
     */
    suspend fun ask(
        text: String,
        lane: String = "web",
        timeout: Duration = 5.minutes,
        onDelta: ((String) -> Unit)? = null,
    ): AskResult = coroutineScope {
        val eventsCall = streamHttp.newCall(
            request(url("events").newBuilder().addQueryParameter("lane", lane).build())
                .header("Accept", "text/event-stream")
                .get()
                .build(),
        )
        val events = Channel<JsonObject>(Channel.UNLIMITED)
        val reader = launch(Dispatchers.IO) { readEvents(eventsCall, events) }

        var stage = Stage.AWAITING_LIVE
        var steered = false
        var settled = false
        val reply = StringBuilder()
        try {
            withTimeoutOrNull(timeout) {
                while (true) {
                    val result = events.receiveCatching()
                    val event = result.getOrNull() ?: throw beforePromptFailure(result.exceptionOrNull())
                    if (event.type == HOST_STREAM_LIVE) break
                }
                stage = Stage.PROMPTING
                steered = prompt(text, lane).steered
                stage = Stage.COLLECTING
                while (true) {
                    val event = events.receiveCatching().getOrNull() ?: break
                    when (event.type) {
                        AGENT_SETTLED -> { settled = true; break }
                        MESSAGE_END, MESSAGE -> assistantText(event)?.let { chunk ->
                            reply.append(chunk)
                            if (chunk.isNotEmpty()) onDelta?.invoke(chunk)
                        }
                    }
                }
            }
        } finally {
            eventsCall.cancel()
            reader.cancel()
        }
        when (stage) {
            Stage.AWAITING_LIVE -> throw SambotException("timed out after $timeout waiting for $HOST_STREAM_LIVE; prompt not sent")
            Stage.PROMPTING -> throw PromptOutcomeUnknownException("timed out after $timeout during POST /prompt")
            Stage.COLLECTING -> AskResult(reply.toString(), steered, partial = !settled)
        }
    }

    private enum class Stage { AWAITING_LIVE, PROMPTING, COLLECTING }

    private suspend fun prompt(text: String, lane: String): PromptResponse = withContext(Dispatchers.IO) {
        val body = JSON.encodeToString(PromptRequest.serializer(), PromptRequest(text, lane)).toRequestBody(JSON_TYPE)
        val response = try {
            http.newCall(request(url("prompt")).post(body).build()).await()
        } catch (e: IOException) {
            throw PromptOutcomeUnknownException("POST /prompt failed: ${e.message}", e)
        }
        response.use {
            if (it.code == 409) throw SambotException("agent is not running", 409)
            if (!it.isSuccessful) throw it.failure("POST /prompt")
            val parsed = it.decode<PromptResponse>("POST /prompt")
            if (!parsed.ok) throw SambotException("POST /prompt answered ok=false", it.code)
            parsed
        }
    }

    /** Blocking SSE reader: one JSON object per frame into [out], then closes it (with the failure, if any). */
    private fun readEvents(call: Call, out: SendChannel<JsonObject>) {
        try {
            call.execute().use { response ->
                if (!response.isSuccessful) throw response.failure("GET /events")
                val source = response.body.source()
                val data = StringBuilder()
                var hasData = false
                while (true) {
                    val line = source.readUtf8Line() ?: break // An unterminated last frame is dropped, as in SSE.
                    when {
                        line.isEmpty() -> {
                            if (hasData) parseEvent(data.toString())?.let { out.trySend(it) }
                            data.setLength(0)
                            hasData = false
                        }
                        line.startsWith("data:") -> {
                            if (hasData) data.append('\n')
                            data.append(line.substring(5).removePrefix(" "))
                            hasData = true
                        }
                        // `event:`, `id:`, `retry:` and `:` comments carry nothing SamBot uses.
                    }
                }
            }
            out.close()
        } catch (e: Throwable) {
            out.close(e)
        }
    }

    private fun beforePromptFailure(cause: Throwable?): SambotException = when (cause) {
        is SambotException -> cause
        null -> SambotException("event stream ended before $HOST_STREAM_LIVE; prompt not sent")
        else -> SambotException("event stream failed before $HOST_STREAM_LIVE; prompt not sent: ${cause.message}", cause = cause)
    }

    private fun url(path: String): HttpUrl = base.newBuilder().addPathSegments(path).build()

    private fun request(url: HttpUrl): Request.Builder =
        Request.Builder().url(url).header(AUTH_HEADER, "Bearer $token")

    private inline fun <reified T> Response.decode(what: String): T = try {
        JSON.decodeFromString<T>(body.string())
    } catch (e: IllegalArgumentException) { // SerializationException is one
        throw SambotException("$what: unexpected response body", code, e)
    }

    private fun Response.failure(what: String): SambotException {
        if (code in 300..399) {
            return SambotException("$what: HTTP $code redirect to ${header("Location")}; is the token missing or invalid?", code)
        }
        val error = runCatching {
            val obj = JSON.parseToJsonElement(body.string()) as? JsonObject
            (obj?.get("error") as? JsonPrimitive)?.contentOrNull
        }.getOrNull()
        return SambotException(if (error != null) "$what: $error" else "$what: HTTP $code", code)
    }

    companion object {
        const val AUTH_HEADER = "X-Exedev-Authorization"
        const val DEFAULT_BASE_URL = "https://sambot.exe.xyz"

        private const val HOST_STREAM_LIVE = "host_stream_live"
        private const val AGENT_SETTLED = "agent_settled"
        private const val MESSAGE_END = "message_end"
        private const val MESSAGE = "message"

        private val OGG = "audio/ogg".toMediaType()
        private val JSON_TYPE = "application/json; charset=utf-8".toMediaType()
        private val JSON = Json { ignoreUnknownKeys = true }

        private fun parseEvent(data: String): JsonObject? =
            runCatching { JSON.parseToJsonElement(data) as? JsonObject }.getOrNull()

        private val JsonObject.type: String? get() = (get("type") as? JsonPrimitive)?.takeIf { it.isString }?.content

        /**
         * The reference rule: message = event.message ?: event; it must have role "assistant"; string
         * content is used as is, array content contributes the `text` of its `type: "text"` blocks.
         */
        internal fun assistantText(event: JsonObject): String? {
            val message = when (val m = event["message"]) {
                null, JsonNull -> event
                is JsonObject -> m
                else -> return null
            }
            if (message.stringField("role") != "assistant") return null
            return when (val content = message["content"]) {
                is JsonPrimitive -> content.takeIf { it.isString }?.content
                is JsonArray -> content.filterIsInstance<JsonObject>()
                    .filter { it.stringField("type") == "text" }
                    .mapNotNull { it.stringField("text") }
                    .joinToString("")
                else -> null
            }
        }

        private fun JsonObject.stringField(key: String): String? =
            (get(key) as? JsonPrimitive)?.takeIf { it.isString }?.content
    }
}

@Serializable
private data class HealthResponse(val ok: Boolean = false)

@Serializable
private data class TranscribeResponse(val text: String)

@Serializable
private data class PromptRequest(val text: String, val lane: String)

@Serializable
private data class PromptResponse(
    val ok: Boolean = false,
    val steered: Boolean = false,
    val lane: String? = null,
    val sessionId: String? = null,
)

/** Enqueues the call and cancels it when the coroutine is cancelled. */
private suspend fun Call.await(): Response = suspendCancellableCoroutine { cont ->
    cont.invokeOnCancellation { cancel() }
    enqueue(object : Callback {
        override fun onFailure(call: Call, e: IOException) = cont.resumeWithException(e)
        override fun onResponse(call: Call, response: Response) =
            cont.resume(response) { _, value, _ -> value.close() }
    })
}
