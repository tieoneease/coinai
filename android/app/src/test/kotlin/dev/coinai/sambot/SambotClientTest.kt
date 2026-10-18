package dev.coinai.sambot

import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonObject
import mockwebserver3.Dispatcher
import mockwebserver3.MockResponse
import mockwebserver3.MockResponseBody
import mockwebserver3.MockWebServer
import mockwebserver3.RecordedRequest
import mockwebserver3.SocketEffect
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.MultipartReader
import okhttp3.OkHttpClient
import okio.Buffer
import okio.BufferedSink
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.time.Duration.Companion.milliseconds
import kotlin.time.Duration.Companion.seconds
import kotlin.time.measureTimedValue

class SambotClientTest {
    private val server = MockWebServer()
    private val http = OkHttpClient.Builder().readTimeout(5, TimeUnit.SECONDS).build()

    /** Lets stalled event streams return when the test ends. */
    private val release = CountDownLatch(1)
    private val promptArrived = CountDownLatch(1)
    private val promptCalls = AtomicInteger()

    /** Writes the `/events` body; runs on the server's connection thread. */
    @Volatile private var events: (Sse) -> Unit = {}
    @Volatile private var eventsResponse: () -> MockResponse = {
        MockResponse.Builder().addHeader("Content-Type", "text/event-stream").body(SseBody(events)).build()
    }
    @Volatile private var promptResponse = json(200, """{"ok":true,"steered":false,"lane":"web","sessionId":"s-1"}""")
    @Volatile private var transcribeResponse = json(200, """{"text":"hello there"}""")
    @Volatile private var healthResponse = json(200, """{"ok":true}""")
    @Volatile private var sceneResponse = json(200, """{"scene": {"say": "Nine.", "card": {"kind": "stat", "value": "9", "label": "20% of 45"}}, "handoff": null, "issues": []}""")

    private lateinit var client: SambotClient

    @Before
    fun setUp() {
        server.dispatcher = object : Dispatcher() {
            override fun dispatch(request: RecordedRequest): MockResponse = when (request.url.encodedPath.removePrefix("/sb")) {
                "/events" -> eventsResponse()
                "/prompt" -> {
                    promptCalls.incrementAndGet()
                    promptArrived.countDown()
                    promptResponse
                }
                "/voice/transcribe" -> transcribeResponse
                "/health" -> healthResponse
                "/coin/scene" -> sceneResponse
                else -> json(404, """{"error":"no route"}""")
            }
        }
        server.start()
        client = SambotClient(server.url("/").toString(), TOKEN, http)
    }

    @After
    fun tearDown() {
        release.countDown()
        server.close()
    }

    // ask: replay boundary and turn collection

    @Test
    fun replayBeforeHostStreamLiveIsIgnored() = runBlocking {
        events = { sse ->
            sse.send(assistantEnd("Old reply from replay"), SETTLED, userEnd("old question"))
            sse.send(LIVE)
            sse.awaitPrompt()
            sse.send(assistantEnd("Fresh reply"), SETTLED)
        }
        assertEquals(AskResult("Fresh reply", steered = false, partial = false), client.ask("hi"))
    }

    @Test
    fun promptIsNotSentBeforeHostStreamLive() = runBlocking {
        val promptsBeforeLive = AtomicInteger(-1)
        events = { sse ->
            sse.send(assistantEnd("replayed"), SETTLED)
            Thread.sleep(400) // Give a wrong client time to post early.
            promptsBeforeLive.set(promptCalls.get())
            sse.send(LIVE)
            sse.awaitPrompt()
            sse.send(assistantEnd("ok"), SETTLED)
        }
        assertEquals("ok", client.ask("hi").text)
        assertEquals(0, promptsBeforeLive.get())
        assertEquals(1, promptCalls.get())
    }

    @Test
    fun laterTurnInTheSameChunkIsNotSpliced() = runBlocking {
        events = { sse ->
            sse.send(LIVE)
            sse.awaitPrompt()
            // One write and flush: this turn's end and the next turn arrive in a single network chunk.
            sse.send(assistantEnd("This turn."), SETTLED, assistantEnd("A later turn."), SETTLED)
        }
        assertEquals(AskResult("This turn.", steered = false, partial = false), client.ask("hi"))
    }

    @Test
    fun onlyAssistantTextBlocksAreCollected() = runBlocking {
        events = { sse ->
            sse.send(LIVE)
            sse.awaitPrompt()
            sse.send(
                userEnd("user words"),
                ev("""{"type":"message_end","message":{"role":"toolResult","content":[{"type":"text","text":"tool output"}]}}"""),
                ev(
                    """{"type":"message_end","message":{"role":"assistant","content":[
                    {"type":"thinking","thinking":"hmm"},{"type":"text","text":"Hello "},
                    {"type":"toolCall","name":"search","arguments":{}},{"type":"text","text":"world"}]}}""".oneLine(),
                ),
                ev("""{"type":"message_update","message":{"role":"assistant","content":"streaming delta"}}"""),
                ev("""{"type":"tool_execution_end","result":{"role":"assistant","content":"nope"}}"""),
                ev("""{"type":"message","role":"assistant","content":"!"}"""), // message = event itself
                ev("""{"type":"message","message":"not an object","role":"assistant","content":"skip"}"""),
                "data: not json\n\n",
                ": comment line\n\n",
                SETTLED,
            )
        }
        val deltas = mutableListOf<String>()
        val result = client.ask("hi", onDelta = { deltas += it })
        assertEquals("Hello world!", result.text)
        assertEquals(listOf("Hello world", "!"), deltas)
        assertEquals(false, result.partial)
    }

    @Test
    fun stringContentIsUsedAsIs() = runBlocking {
        events = { sse ->
            sse.send(LIVE)
            sse.awaitPrompt()
            sse.send(ev("""{"type":"message_end","message":{"role":"assistant","content":"Plain string reply"}}"""), SETTLED)
        }
        assertEquals("Plain string reply", client.ask("hi").text)
    }

    @Test
    fun multiLineDataAndCrLfFramesParse() = runBlocking {
        events = { sse ->
            sse.send(LIVE)
            sse.awaitPrompt()
            sse.send(
                "event: x\r\ndata: {\"type\":\"message_end\",\r\ndata: \"message\":{\"role\":\"assistant\",\"content\":\"split\"}}\r\n\r\n",
                SETTLED,
            )
        }
        assertEquals("split", client.ask("hi").text)
    }

    @Test
    fun timeoutReturnsPartial() = runBlocking {
        promptResponse = json(200, """{"ok":true,"steered":true,"lane":"web","sessionId":"s-1"}""")
        events = { sse ->
            sse.send(LIVE)
            sse.awaitPrompt()
            sse.send(assistantEnd("Partial "))
            sse.stall()
        }
        val (result, elapsed) = measureTimedValue { client.ask("hi", timeout = 1.seconds) }
        assertEquals(AskResult("Partial", steered = true, partial = true), result)
        assertTrue("took $elapsed", elapsed < 4.seconds)
    }

    @Test
    fun streamEndReturnsPartial() = runBlocking {
        events = { sse ->
            sse.send(LIVE)
            sse.awaitPrompt()
            sse.send(assistantEnd("Cut off"))
        }
        eventsResponse = {
            MockResponse.Builder().addHeader("Content-Type", "text/event-stream").body(SseBody(events))
                .onResponseEnd(SocketEffect.CloseSocket())
                .build()
        }
        val (result, elapsed) = measureTimedValue { client.ask("hi", timeout = 10.seconds) }
        assertEquals(AskResult("Cut off", steered = false, partial = true), result)
        assertTrue("took $elapsed", elapsed < 5.seconds)
    }

    @Test
    fun timeoutBeforeHostStreamLiveThrowsWithoutPrompting() {
        events = { sse ->
            sse.send(assistantEnd("replayed"), SETTLED)
            sse.stall()
        }
        val e = assertThrows(SambotException::class.java) { runBlocking { client.ask("hi", timeout = 500.milliseconds) } }
        assertTrue(e.message!!, e.message!!.contains("prompt not sent"))
        assertEquals(0, promptCalls.get())
    }

    @Test
    fun eventStreamHttpErrorThrowsWithoutPrompting() {
        eventsResponse = { json(401, """{"error":"bad token"}""") }
        val e = assertThrows(PromptNotSentException::class.java) { runBlocking { client.ask("hi") } }
        assertEquals(401, e.status)
        assertTrue(e.message!!, e.message!!.contains("bad token"))
        assertEquals(0, promptCalls.get())
    }

    @Test
    fun prompt409MeansAgentNotRunning() {
        promptResponse = json(409, """{"error":"conflict"}""")
        events = { sse ->
            sse.send(LIVE)
            sse.stall()
        }
        val e = assertThrows(SambotException::class.java) { runBlocking { client.ask("hi") } }
        assertEquals("agent is not running", e.message)
        assertEquals(409, e.status)
        assertFalse(e is PromptOutcomeUnknownException || e is PromptNotSentException)
    }

    @Test
    fun prompt5xxOrUnreadableAnswerHasAnUnknownOutcome() {
        events = { sse ->
            sse.send(LIVE)
            sse.stall()
        }
        promptResponse = json(502, """{"error":"upstream timeout"}""")
        val e = assertThrows(PromptOutcomeUnknownException::class.java) { runBlocking { client.ask("hi") } }
        assertTrue(e.message!!, e.message!!.contains("upstream timeout"))

        promptResponse = MockResponse.Builder().code(200).body("<html>proxy</html>").build()
        assertThrows(PromptOutcomeUnknownException::class.java) { runBlocking { client.ask("hi") } }
        assertEquals(2, promptCalls.get())
    }

    @Test
    fun askRequestsHaveTheReferenceShape() = runBlocking {
        events = { sse ->
            sse.send(LIVE)
            sse.awaitPrompt()
            sse.send(SETTLED)
        }
        assertEquals(AskResult("", steered = false, partial = false), client.ask("turn on the lights", lane = "web"))
        val requests = drain()
        val eventsRequest = requests.single { it.url.encodedPath == "/events" }
        assertEquals("GET", eventsRequest.method)
        assertEquals("web", eventsRequest.url.queryParameter("lane"))
        assertEquals("1", eventsRequest.url.queryParameter("tail")) // not the whole transcript
        assertEquals("text/event-stream", eventsRequest.headers["Accept"])
        val prompt = requests.single { it.url.encodedPath == "/prompt" }
        assertEquals("POST", prompt.method)
        assertTrue(prompt.headers["Content-Type"]!!.startsWith("application/json"))
        assertEquals(
            Json.parseToJsonElement("""{"text":"turn on the lights","lane":"web"}"""),
            Json.parseToJsonElement(prompt.body!!.utf8()),
        )
    }

    // Auth header

    @Test
    fun everyCallSendsTheExeDevAuthorizationHeader() = runBlocking {
        events = { sse ->
            sse.send(LIVE)
            sse.awaitPrompt()
            sse.send(assistantEnd("done"), SETTLED)
        }
        client.health()
        client.transcribe(byteArrayOf(1, 2, 3))
        client.ask("hi")
        val requests = drain()
        assertEquals(
            setOf("/health", "/voice/transcribe", "/events", "/prompt"),
            requests.map { it.url.encodedPath }.toSet(),
        )
        for (r in requests) assertEquals(r.requestLine, "Bearer $TOKEN", r.headers["X-Exedev-Authorization"])
    }

    // transcribe

    @Test
    fun transcribeSendsMultipartAudioPart() = runBlocking {
        val ogg = "OggS".toByteArray() + ByteArray(300) { it.toByte() }
        assertEquals("hello there", client.transcribe(ogg))

        val request = drain().single()
        assertEquals("POST", request.method)
        assertEquals("/voice/transcribe", request.url.encodedPath)
        val contentType = request.headers["Content-Type"]!!.toMediaType()
        assertEquals("multipart/form-data", "${contentType.type}/${contentType.subtype}")
        MultipartReader(Buffer().write(request.body!!), contentType.parameter("boundary")!!).use { reader ->
            val part = reader.nextPart()!!
            assertEquals("""form-data; name="audio"; filename="clip.ogg"""", part.headers["Content-Disposition"])
            assertEquals("audio/ogg", part.headers["Content-Type"])
            assertArrayEquals(ogg, part.body.readByteArray())
            part.body.close()
            assertNull(reader.nextPart())
        }
    }

    @Test
    fun transcribeErrorCarriesTheErrorField() {
        transcribeResponse = json(502, """{"error":"groq: upstream timeout"}""")
        val e = assertThrows(SambotException::class.java) { runBlocking { client.transcribe(byteArrayOf(1)) } }
        assertEquals(502, e.status)
        assertTrue(e.message!!, e.message!!.contains("groq: upstream timeout"))

        transcribeResponse = MockResponse.Builder().code(500).body("<html>oops</html>").build()
        val plain = assertThrows(SambotException::class.java) { runBlocking { client.transcribe(byteArrayOf(1)) } }
        assertTrue(plain.message!!, plain.message!!.contains("HTTP 500"))
    }

    // scene

    @Test
    fun sceneSendsTextTimeAndOffsetAndKeepsTheSceneCompact() = runBlocking {
        val result = client.scene("what is 20% of 45", atMs = 1_760_000_000_000, utcOffsetMin = -300)

        assertEquals("""{"say":"Nine.","card":{"kind":"stat","value":"9","label":"20% of 45"}}""", result.scene)
        assertNull(result.handoff)
        assertEquals(emptyList<String>(), result.issues)
        val request = drain().single()
        assertEquals("POST", request.method)
        assertEquals("/coin/scene", request.url.encodedPath)
        assertEquals("Bearer $TOKEN", request.headers["X-Exedev-Authorization"])
        assertEquals(
            Json.parseToJsonElement("""{"text":"what is 20% of 45","at_ms":1760000000000,"utc_offset_min":-300}"""),
            Json.parseToJsonElement(request.body!!.utf8()),
        )
    }

    @Test
    fun sceneLeavesOutUnknownTimeAndReadsTheHandoff() = runBlocking {
        sceneResponse = json(
            200,
            """{"scene":{"say":"On it.","mood":"focus","card":{"kind":"later","text":"Remind you at 5"}},"handoff":"remind me at 5","issues":["model: timeout"],"extra":1}""",
        )
        val result = client.scene("remind me at 5")

        assertEquals("remind me at 5", result.handoff)
        assertEquals(listOf("model: timeout"), result.issues)
        assertEquals("later", Json.parseToJsonElement(result.scene).jsonObject["card"]!!.jsonObject["kind"]!!.let { (it as JsonPrimitive).content })
        assertEquals("""{"text":"remind me at 5"}""", drain().single().body!!.utf8())
    }

    @Test
    fun sceneMalformedBodiesAndHttpErrorsThrow() {
        val bad = listOf(
            "not json",
            "[]",
            """{"handoff":null,"issues":[]}""",
            """{"scene":"text","handoff":null,"issues":[]}""",
            """{"scene":{"card":{}},"handoff":42,"issues":[]}""",
            """{"scene":{"card":{}},"handoff":null,"issues":"none"}""",
        )
        for (body in bad) {
            sceneResponse = json(200, body)
            val e = assertThrows(body, SambotException::class.java) { runBlocking { client.scene("hi") } }
            assertEquals(body, 200, e.status)
        }

        sceneResponse = json(503, """{"error":"no model gateway"}""")
        val unavailable = assertThrows(SambotException::class.java) { runBlocking { client.scene("hi") } }
        assertEquals(503, unavailable.status)
        assertTrue(unavailable.message!!, unavailable.message!!.contains("no model gateway"))

        sceneResponse = json(204, "")
        assertEquals(204, assertThrows(SambotException::class.java) { runBlocking { client.scene("hi") } }.status)

        sceneResponse = MockResponse.Builder().onResponseStart(SocketEffect.CloseSocket()).build()
        assertThrows(java.io.IOException::class.java) { runBlocking { client.scene("hi") } }
    }

    // health and URLs

    @Test
    fun healthReadsOkField() = runBlocking {
        assertEquals(true, client.health())
        healthResponse = json(200, """{"ok":false}""")
        assertEquals(false, client.health())
    }

    @Test
    fun loginRedirectIsReportedNotFollowed() {
        healthResponse = MockResponse.Builder().code(302).addHeader("Location", server.url("/login")).build()
        val e = assertThrows(SambotException::class.java) { runBlocking { client.health() } }
        assertEquals(302, e.status)
        assertTrue(e.message!!, e.message!!.contains("token"))
        assertEquals(listOf("/health"), drain().map { it.url.encodedPath })
    }

    @Test
    fun baseUrlPathPrefixIsKept() = runBlocking {
        assertTrue(SambotClient(server.url("/sb").toString(), TOKEN, http).health())
        assertTrue(SambotClient(server.url("/sb/").toString(), TOKEN, http).health())
        assertEquals(listOf("/sb/health", "/sb/health"), drain().map { it.url.encodedPath })
        assertThrows(IllegalArgumentException::class.java) { SambotClient("sambot.exe.xyz", TOKEN, http) }
        Unit
    }

    @Test
    fun assistantTextRule() {
        fun text(json: String) = SambotClient.assistantText(Json.parseToJsonElement(json).jsonObject)
        assertEquals("a", text("""{"message":{"role":"assistant","content":"a"}}"""))
        assertEquals("a", text("""{"message":null,"role":"assistant","content":"a"}"""))
        assertNull(text("""{"message":{"role":"user","content":"a"}}"""))
        assertNull(text("""{"message":{"role":"assistant","content":42}}"""))
        assertNull(text("""{"message":{"role":"assistant"}}"""))
        assertEquals("", text("""{"message":{"role":"assistant","content":[{"type":"image"}]}}"""))
        assertEquals("xy", text("""{"message":{"role":"assistant","content":[{"type":"text","text":"x"},"junk",{"type":"text","text":"y"}]}}"""))
    }

    private fun drain(): List<RecordedRequest> =
        generateSequence { server.takeRequest(100, TimeUnit.MILLISECONDS) }.toList()

    private inner class Sse(private val sink: BufferedSink) {
        /** Writes [frames] and flushes once, so they leave as one network chunk. */
        fun send(vararg frames: String) {
            for (f in frames) sink.writeUtf8(f)
            sink.flush()
        }

        fun awaitPrompt() = check(promptArrived.await(5, TimeUnit.SECONDS)) { "prompt never arrived" }
        fun stall() {
            release.await(15, TimeUnit.SECONDS)
        }
    }

    private inner class SseBody(private val script: (Sse) -> Unit) : MockResponseBody {
        override val contentLength: Long = -1L
        override fun writeTo(sink: BufferedSink) = script(Sse(sink))
    }

    private companion object {
        const val TOKEN = "test-token"
        val LIVE = ev("""{"type":"host_stream_live"}""")
        val SETTLED = ev("""{"type":"agent_settled"}""")

        fun ev(json: String) = "data: $json\n\n"
        fun assistantEnd(text: String) = ev(message("assistant", text))
        fun userEnd(text: String) = ev(message("user", text))
        fun message(role: String, text: String): String = JsonObject(
            mapOf(
                "type" to JsonPrimitive("message_end"),
                "message" to JsonObject(
                    mapOf(
                        "role" to JsonPrimitive(role),
                        "content" to Json.parseToJsonElement("""[{"type":"text","text":${JsonPrimitive(text)}}]"""),
                    ),
                ),
            ),
        ).toString()

        fun json(code: Int, body: String) =
            MockResponse.Builder().code(code).addHeader("Content-Type", "application/json").body(body).build()

        fun String.oneLine() = lines().joinToString("") { it.trim() }
    }
}
