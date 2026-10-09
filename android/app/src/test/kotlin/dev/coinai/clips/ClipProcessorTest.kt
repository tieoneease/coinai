package dev.coinai.clips

import dev.coinai.protocol.ReplyState
import dev.coinai.sambot.SambotClient
import dev.coinai.testing.MemoryClips
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import mockwebserver3.Dispatcher
import mockwebserver3.MockResponse
import mockwebserver3.MockResponseBody
import mockwebserver3.MockWebServer
import mockwebserver3.RecordedRequest
import mockwebserver3.SocketEffect
import okhttp3.OkHttpClient
import okio.BufferedSink
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.time.Duration.Companion.seconds

class ClipProcessorTest {
    @get:Rule val tmp = TemporaryFolder()

    private val server = MockWebServer()
    private val http = OkHttpClient.Builder().readTimeout(5, TimeUnit.SECONDS).build()
    private val release = CountDownLatch(1)
    private val prompts = AtomicInteger()
    private val transcribes = AtomicInteger()
    private val promptBodies = CopyOnWriteArrayList<String>()

    @Volatile private var transcribeResponse = json(200, """{"text":"turn on the lights"}""")
    @Volatile private var promptResponse: () -> MockResponse = { json(200, """{"ok":true,"steered":false,"lane":"web","sessionId":"s"}""") }
    @Volatile private var replyText = "**Done.** The lights are on."
    /** When set, GET /events answers with this response instead of a stream. */
    @Volatile private var eventsError: MockResponse? = null

    private val store by lazy { MemoryClips(tmp.root) }
    private val notified = CopyOnWriteArrayList<Pair<Long, String>>()
    private val coin = CopyOnWriteArrayList<CoinReply>()
    private var settings = ProcessingSettings(askTimeout = 5.seconds)

    private val processor by lazy {
        ClipProcessor(
            store = store,
            sambot = { SambotClient(server.url("/").toString(), "tok", http) },
            settings = { settings },
            notifier = { clip, reply -> notified += clip.id to reply },
            replies = { coin += it },
        )
    }

    @Before
    fun setUp() {
        // Each turn's frames go out only after this turn's prompt arrived, as SamBot does.
        val promptSeen = java.util.concurrent.Semaphore(0)
        server.dispatcher = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest): MockResponse = when (request.url.encodedPath) {
                "/voice/transcribe" -> { transcribes.incrementAndGet(); transcribeResponse }
                "/prompt" -> {
                    prompts.incrementAndGet()
                    promptBodies += request.body!!.utf8()
                    promptSeen.release()
                    promptResponse()
                }
                "/events" -> eventsError ?: MockResponse.Builder()
                    .addHeader("Content-Type", "text/event-stream")
                    .body(object : MockResponseBody {
                        override val contentLength = -1L
                        override fun writeTo(sink: BufferedSink) {
                            sink.writeUtf8(ev("""{"type":"host_stream_live"}""")).flush()
                            if (!promptSeen.tryAcquire(5, TimeUnit.SECONDS)) return
                            val content = Json.encodeToString(kotlinx.serialization.json.JsonPrimitive.serializer(), kotlinx.serialization.json.JsonPrimitive(replyText))
                            sink.writeUtf8(ev("""{"type":"message_end","message":{"role":"assistant","content":$content}}"""))
                            sink.writeUtf8(ev("""{"type":"agent_settled"}""")).flush()
                            release.await(5, TimeUnit.SECONDS)
                        }
                    }).build()
                else -> json(404, "{}")
            }
        }
        server.start()
    }

    @After
    fun tearDown() {
        release.countDown()
        server.close()
    }

    private fun received(voicedMs: Long = 2000, state: ClipState = ClipState.RECEIVED, transcript: String? = null): ClipRecord {
        val file = File(tmp.root, "clip-${System.nanoTime()}.ogg").apply { writeBytes("OggS-fake-audio".toByteArray()) }
        return store.insert(
            ClipRecord(0, "C0:A1", 7, state, size = file.length(), durationMs = 3000, voicedMs = voicedMs, filePath = file.path, transcript = transcript),
        )
    }

    private fun state(id: Long) = store.rows.getValue(id).state

    @Test
    fun receivedClipIsTranscribedPromptedAndReplied() = runBlocking {
        val clip = received()

        assertEquals(ClipProcessor.Outcome.Done, processor.process(clip.id))

        val row = store.rows.getValue(clip.id)
        assertEquals(ClipState.REPLIED, row.state)
        assertEquals("turn on the lights", row.transcript)
        assertEquals("**Done.** The lights are on.", row.reply)
        assertFalse(row.replyPartial)
        assertEquals(1, prompts.get())
        assertEquals("turn on the lights", Json.parseToJsonElement(promptBodies.single()).jsonObject["text"]!!.jsonPrimitive.content)
        assertEquals(listOf(clip.id to "**Done.** The lights are on."), notified.toList())
        assertEquals(listOf(ReplyState.TRANSCRIBING, ReplyState.THINKING, ReplyState.DONE), coin.map { it.state })
        assertEquals(7, coin.last().clipId)
    }

    @Test
    fun lowVoicedTimeIsNoSpeechWithoutAnyRequest() = runBlocking {
        val clip = received(voicedMs = 399)

        processor.process(clip.id)

        assertEquals(ClipState.NO_SPEECH, state(clip.id))
        assertEquals(0, server.requestCount)
        assertEquals(listOf(ReplyState.FAILED), coin.map { it.state })
        assertTrue(notified.isEmpty())
    }

    @Test
    fun voicedThresholdComesFromSettings() = runBlocking {
        settings = settings.copy(voicedThresholdMs = 3000)
        val clip = received(voicedMs = 2500)

        processor.process(clip.id)

        assertEquals(ClipState.NO_SPEECH, state(clip.id))
        assertEquals(0, server.requestCount)
    }

    @Test
    fun blankTranscriptIsNoSpeechAndNeverPrompted() = runBlocking {
        transcribeResponse = json(200, """{"text":"   "}""")
        val clip = received()

        processor.process(clip.id)

        assertEquals(ClipState.NO_SPEECH, state(clip.id))
        assertEquals(1, transcribes.get())
        assertEquals(0, prompts.get())
    }

    @Test
    fun transcribeFailureIsRetriedThenSucceeds() = runBlocking {
        transcribeResponse = json(503, """{"error":"groq down"}""")
        val clip = received()

        val first = processor.process(clip.id, attempt = 0)
        assertTrue(first is ClipProcessor.Outcome.Retry)
        assertEquals(ClipState.RECEIVED, state(clip.id))
        assertEquals(0, prompts.get())

        transcribeResponse = json(200, """{"text":"hello"}""")
        assertEquals(ClipProcessor.Outcome.Done, processor.process(clip.id, attempt = 1))
        assertEquals(ClipState.REPLIED, state(clip.id))
        assertEquals(2, transcribes.get())
    }

    @Test
    fun transcribeClientErrorFailsAtOnceAndRetriesAreBounded() = runBlocking {
        transcribeResponse = json(400, """{"error":"not audio"}""")
        val bad = received()
        assertEquals(ClipProcessor.Outcome.Done, processor.process(bad.id))
        assertEquals(ClipState.FAILED, state(bad.id))

        transcribeResponse = json(503, "{}")
        val flaky = received()
        assertEquals(ClipProcessor.Outcome.Done, processor.process(flaky.id, attempt = settings.maxAttempts - 1))
        assertEquals(ClipState.FAILED, state(flaky.id))
    }

    @Test
    fun unknownPromptOutcomeIsUncertainAndNeverRetried() = runBlocking {
        // The prompt reaches the server, the answer is lost.
        promptResponse = { MockResponse.Builder().onResponseStart(SocketEffect.CloseSocket()).build() }
        val clip = received()

        assertEquals(ClipProcessor.Outcome.Done, processor.process(clip.id))
        assertEquals(ClipState.UNCERTAIN, state(clip.id))
        assertEquals(ReplyState.FAILED, coin.last().state)

        processor.process(clip.id)
        processor.process(clip.id)
        assertEquals(ClipState.UNCERTAIN, state(clip.id))
        assertEquals("prompted once", 1, prompts.get())
        assertTrue(notified.isEmpty())
    }

    @Test
    fun crashAfterClaimBecomesUncertain() = runBlocking {
        val clip = received(state = ClipState.PROMPTED, transcript = "hi")

        processor.process(clip.id)

        assertEquals(ClipState.UNCERTAIN, state(clip.id))
        assertEquals(0, server.requestCount)
    }

    @Test
    fun concurrentRunsPromptAtMostOnce() = runBlocking {
        val clip = received(state = ClipState.TRANSCRIBED, transcript = "only once")

        (1..4).map { async(Dispatchers.IO) { processor.process(clip.id) } }.awaitAll()

        assertEquals(1, prompts.get())
        assertEquals(1, store.claims)
        assertEquals(ClipState.REPLIED, state(clip.id))
    }

    @Test
    fun promptNotSentIsReleasedAndRetried() = runBlocking {
        eventsError = json(503, """{"error":"restarting"}""")
        val clip = received(state = ClipState.TRANSCRIBED, transcript = "hello")

        val outcome = processor.process(clip.id)

        assertTrue(outcome is ClipProcessor.Outcome.Retry)
        assertEquals(ClipState.TRANSCRIBED, state(clip.id))
        assertEquals(0, prompts.get())

        // A refused event stream (bad token) will not change on retry.
        eventsError = json(401, """{"error":"bad token"}""")
        assertEquals(ClipProcessor.Outcome.Done, processor.process(clip.id))
        assertEquals(ClipState.FAILED, state(clip.id))
        assertEquals(0, prompts.get())
    }

    @Test
    fun refusedPromptFailsAndManualResendPromptsAgain() = runBlocking {
        promptResponse = { json(409, """{"error":"not running"}""") }
        val clip = received(state = ClipState.TRANSCRIBED, transcript = "hello")

        processor.process(clip.id)
        assertEquals(ClipState.FAILED, state(clip.id))
        assertEquals("agent is not running", store.rows.getValue(clip.id).error)

        promptResponse = { json(200, """{"ok":true,"steered":true,"lane":"web","sessionId":"s"}""") }
        assertTrue(processor.resend(clip.id))
        assertEquals(ClipState.TRANSCRIBED, state(clip.id))
        processor.process(clip.id)
        assertEquals(ClipState.REPLIED, state(clip.id))
        assertEquals(2, prompts.get())
        assertEquals("the kept transcript is reused", 0, transcribes.get())
    }

    @Test
    fun resendOfNoSpeechSkipsTheVoicedGateOnce() = runBlocking {
        val clip = received(voicedMs = 100)
        processor.process(clip.id)
        assertEquals(ClipState.NO_SPEECH, state(clip.id))

        assertTrue(processor.resend(clip.id))
        processor.process(clip.id)

        assertEquals(ClipState.REPLIED, state(clip.id))
        assertEquals(1, transcribes.get())
        assertEquals(1, prompts.get())
    }

    @Test
    fun repliedClipsCannotBeResent() = runBlocking {
        val clip = received()
        processor.process(clip.id)
        assertEquals(ClipState.REPLIED, state(clip.id))
        assertFalse(processor.resend(clip.id))
        processor.process(clip.id)
        assertEquals(1, prompts.get())
    }

    private companion object {
        fun ev(json: String) = "data: $json\n\n"
        fun json(code: Int, body: String) =
            MockResponse.Builder().code(code).addHeader("Content-Type", "application/json").body(body).build()
    }
}
