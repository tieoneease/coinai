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
import org.junit.Assert.assertNull
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
    private val sceneBodies = CopyOnWriteArrayList<String>()
    /** The coin's STATUS bit 3, as the processor sees it. */
    @Volatile private var scenes = false
    @Volatile private var sceneResponse = json(200, STAT_SCENE)
    private val handoffFailures = CopyOnWriteArrayList<Pair<Long, String>>()
    private val logs = CopyOnWriteArrayList<String>()

    @Volatile private var transcribeResponse = json(200, """{"text":"turn on the lights"}""")
    @Volatile private var promptResponse: () -> MockResponse = { json(200, """{"ok":true,"steered":false,"lane":"web","sessionId":"s"}""") }
    @Volatile private var replyText = "**Done.** The lights are on."
    /** When set, GET /events answers with this response instead of a stream. */
    @Volatile private var eventsError: MockResponse? = null

    private val store by lazy { MemoryClips(tmp.root) }
    private val notified = CopyOnWriteArrayList<Pair<Long, String>>()
    private val coin = CopyOnWriteArrayList<CoinReply>()
    private var settings = ProcessingSettings(askTimeout = 5.seconds)
    /** Null stands for unusable SamBot settings: the app's factory then throws IllegalArgumentException. */
    @Volatile private var token: String? = "tok"

    private val processor by lazy {
        ClipProcessor(
            store = store,
            sambot = { SambotClient(server.url("/").toString(), requireNotNull(token) { "no token set" }, http) },
            settings = { settings },
            notifier = object : ReplyNotifier {
                override fun replyReady(clip: ClipRecord, reply: String) {
                    notified += clip.id to reply
                }

                override fun handoffFailed(clip: ClipRecord, reason: String) {
                    handoffFailures += clip.id to reason
                }
            },
            replies = { coin += it },
            showsScenes = { scenes },
            timeZone = { java.util.TimeZone.getTimeZone("America/New_York") },
            log = { logs += it },
        )
    }

    @Before
    fun setUp() {
        // Each turn's frames go out only after this turn's prompt arrived, as SamBot does.
        val promptSeen = java.util.concurrent.Semaphore(0)
        server.dispatcher = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest): MockResponse = when (request.url.encodedPath) {
                "/voice/transcribe" -> { transcribes.incrementAndGet(); transcribeResponse }
                "/coin/scene" -> { sceneBodies += request.body!!.utf8(); sceneResponse }
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
            ClipRecord(
                0, "C0:A1", 7, state, size = file.length(), durationMs = 3000, voicedMs = voicedMs, filePath = file.path,
                transcript = transcript, startedAtMs = STARTED_AT,
            ),
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
        assertEquals("thinking shows what SamBot heard", "turn on the lights", coin[1].text)
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
    fun unusableSettingsFailTheClipOnTheCoinWithoutAnyRequest() = runBlocking {
        token = null
        val clip = received()
        processor.process(clip.id)
        assertEquals(ClipState.FAILED, state(clip.id))
        assertEquals(0, server.requestCount)
        assertEquals(listOf(ReplyState.FAILED), coin.map { it.state })
        assertEquals(ClipProcessor.SETUP_TEXT, coin.single().text)

        // A transcript saved by an earlier run goes the same way at the prompt step.
        coin.clear()
        val heard = received(state = ClipState.TRANSCRIBED, transcript = "hello")
        processor.process(heard.id)
        assertEquals(ClipState.FAILED, state(heard.id))
        assertEquals(listOf(ReplyState.FAILED), coin.map { it.state })
    }

    @Test
    fun refusedTokenSaysSoOnTheCoinOnce() = runBlocking {
        transcribeResponse = json(401, """{"error":"unauthorized"}""")
        val clip = received()
        assertEquals(ClipProcessor.Outcome.Done, processor.process(clip.id))
        assertEquals(ClipState.FAILED, state(clip.id))
        assertEquals("transcribe: unauthorized", store.rows.getValue(clip.id).error)
        assertEquals(ClipProcessor.TOKEN_TEXT, coin.last().text)

        transcribeResponse = MockResponse.Builder().code(302).addHeader("Location", "https://exe.dev/login").build()
        val redirected = received()
        processor.process(redirected.id)
        assertEquals(ClipProcessor.TOKEN_TEXT, coin.last().text)

        transcribeResponse = json(400, """{"error":"not audio"}""")
        val bad = received()
        processor.process(bad.id)
        assertEquals("Failed: transcribe: not audio", coin.last().text)
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

    // Scenes (PROTOCOL.md "Scenes")

    @Test
    fun localSceneAnswersTheCoinWithoutAPrompt() = runBlocking {
        scenes = true
        val clip = received()

        assertEquals(ClipProcessor.Outcome.Done, processor.process(clip.id))

        val row = store.rows.getValue(clip.id)
        assertEquals(ClipState.REPLIED, row.state)
        assertEquals(Json.parseToJsonElement(STAT_SCENE).jsonObject["scene"].toString(), row.scene)
        assertEquals("9\n20% of 45", row.reply)
        assertNull(row.handoff)
        assertEquals(0, prompts.get())
        assertTrue("no notification for a card the coin shows", notified.isEmpty())
        assertEquals(listOf(ReplyState.TRANSCRIBING, ReplyState.THINKING, ReplyState.DONE), coin.map { it.state })
        assertEquals("turn on the lights", coin[1].text)
        assertEquals(row.scene, coin.last().scene)
        assertEquals("9\n20% of 45", coin.last().text)
        // at_ms is the recording start; the offset is the phone's at that instant (EST, not EDT).
        val body = Json.parseToJsonElement(sceneBodies.single()).jsonObject
        assertEquals("turn on the lights", body["text"]!!.jsonPrimitive.content)
        assertEquals(STARTED_AT, body["at_ms"]!!.jsonPrimitive.content.toLong())
        assertEquals(-300, body["utc_offset_min"]!!.jsonPrimitive.content.toInt())
    }

    @Test
    fun laterSceneHandsOffExactlyOnceAndTheCoinGetsOnlyTheScene() = runBlocking {
        scenes = true
        sceneResponse = json(200, LATER_SCENE)
        replyText = "Reminder set for 5 pm."
        val clip = received()

        assertEquals(ClipProcessor.Outcome.Done, processor.process(clip.id))

        val row = store.rows.getValue(clip.id)
        assertEquals(ClipState.REPLIED, row.state)
        assertEquals("Reminder set for 5 pm.", row.reply)
        assertEquals("remind me at 5", row.handoff)
        assertEquals(1, prompts.get())
        assertEquals("remind me at 5", Json.parseToJsonElement(promptBodies.single()).jsonObject["text"]!!.jsonPrimitive.content)
        assertEquals(listOf(clip.id to "Reminder set for 5 pm."), notified.toList())
        assertEquals(listOf(ReplyState.TRANSCRIBING, ReplyState.THINKING, ReplyState.DONE), coin.map { it.state })
        assertEquals("Remind you at 5", coin.last().text)
        assertTrue(coin.last().scene!!.contains("\"later\""))

        processor.process(clip.id)
        assertEquals("never prompted again", 1, prompts.get())
        assertEquals(1, sceneBodies.size)
    }

    @Test
    fun handoffNotSentIsRetriedWithNothingMoreToTheCoin() = runBlocking {
        scenes = true
        sceneResponse = json(200, LATER_SCENE)
        eventsError = json(503, """{"error":"restarting"}""")
        val clip = received()

        assertTrue(processor.process(clip.id, attempt = 0) is ClipProcessor.Outcome.Retry)
        assertEquals(ClipState.TRANSCRIBED, state(clip.id))
        assertEquals("remind me at 5", store.rows.getValue(clip.id).handoff)
        assertEquals(0, prompts.get())
        val shown = coin.toList()
        assertEquals(listOf(ReplyState.TRANSCRIBING, ReplyState.THINKING, ReplyState.DONE), shown.map { it.state })

        eventsError = null
        assertEquals(ClipProcessor.Outcome.Done, processor.process(clip.id, attempt = 1))
        assertEquals(ClipState.REPLIED, state(clip.id))
        assertEquals("the hand-off, not the transcript", "remind me at 5", Json.parseToJsonElement(promptBodies.single()).jsonObject["text"]!!.jsonPrimitive.content)
        assertEquals("composed once", 1, sceneBodies.size)
        assertEquals("nothing more to the coin", shown, coin.toList())
        assertEquals(1, notified.size)
        assertTrue(handoffFailures.isEmpty())
    }

    @Test
    fun handoffThatFinallyFailsNotifiesInsteadOfTheCoin() = runBlocking {
        scenes = true
        sceneResponse = json(200, LATER_SCENE)
        eventsError = json(503, """{"error":"restarting"}""")
        val clip = received()
        processor.process(clip.id, attempt = 0)
        val shown = coin.toList()

        assertEquals(ClipProcessor.Outcome.Done, processor.process(clip.id, attempt = settings.maxAttempts - 1))
        assertEquals(ClipState.FAILED, state(clip.id))
        assertEquals(shown, coin.toList())
        assertEquals(clip.id, handoffFailures.single().first)
        assertTrue(notified.isEmpty())

        // An unknown outcome is UNCERTAIN, and notified the same way.
        eventsError = null
        promptResponse = { MockResponse.Builder().onResponseStart(SocketEffect.CloseSocket()).build() }
        val other = received()
        processor.process(other.id)
        assertEquals(ClipState.UNCERTAIN, state(other.id))
        assertEquals(other.id to ClipProcessor.UNCERTAIN_TEXT, handoffFailures.last())
        assertTrue(coin.none { it.state == ReplyState.FAILED })
    }

    @Test
    fun sceneRouteFailureFallsBackToThePlainPrompt() = runBlocking {
        scenes = true
        sceneResponse = json(503, """{"error":"no model gateway"}""")
        val clip = received()

        assertEquals(ClipProcessor.Outcome.Done, processor.process(clip.id))

        val row = store.rows.getValue(clip.id)
        assertEquals(ClipState.REPLIED, row.state)
        assertEquals("**Done.** The lights are on.", row.reply)
        assertNull(row.scene)
        assertEquals(1, sceneBodies.size)
        assertEquals("turn on the lights", Json.parseToJsonElement(promptBodies.single()).jsonObject["text"]!!.jsonPrimitive.content)
        assertEquals(listOf(ReplyState.TRANSCRIBING, ReplyState.THINKING, ReplyState.DONE), coin.map { it.state })
        assertTrue(coin.all { it.scene == null })
        assertEquals(1, notified.size)
        assertTrue(logs.toString(), logs.any { "no scene" in it && "no model gateway" in it })

        // A malformed 200 falls back the same way.
        sceneResponse = json(200, """{"scene":null,"handoff":null,"issues":[]}""")
        val other = received()
        processor.process(other.id)
        assertEquals(ClipState.REPLIED, state(other.id))
        assertEquals(2, prompts.get())
    }

    @Test
    fun coinWithoutScenesNeverCallsTheComposer() = runBlocking {
        val clip = received()

        processor.process(clip.id)

        assertEquals(ClipState.REPLIED, state(clip.id))
        assertEquals(0, sceneBodies.size)
        assertEquals(1, prompts.get())
        assertEquals(listOf(ReplyState.TRANSCRIBING, ReplyState.THINKING, ReplyState.DONE), coin.map { it.state })
    }


    private companion object {
        /** 2026-01-15 12:00 UTC: EST, UTC-5. */
        const val STARTED_AT = 1_768_478_400_000L
        const val STAT_SCENE = """{"scene":{"say":"Nine. Easy one.","mood":"happy","card":{"kind":"stat","value":"9","label":"20% of 45"}},"handoff":null,"issues":[]}"""
        const val LATER_SCENE = """{"scene":{"say":"On it.","mood":"focus","card":{"kind":"later","text":"Remind you at 5"}},"handoff":"remind me at 5","issues":[]}"""

        fun ev(json: String) = "data: $json\n\n"
        fun json(code: Int, body: String) =
            MockResponse.Builder().code(code).addHeader("Content-Type", "application/json").body(body).build()
    }
}
