package dev.coinai.data

import androidx.room3.Room
import androidx.sqlite.driver.AndroidSQLiteDriver
import androidx.test.core.app.ApplicationProvider
import android.content.Context
import dev.coinai.clips.ClipState
import dev.coinai.protocol.CtrlMessage
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/** The Room schema and queries on Robolectric's real SQLite. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [36])
class ClipRepositoryTest {
    @get:Rule val tmp = TemporaryFolder()

    private lateinit var db: ClipDatabase
    private lateinit var repo: ClipRepository
    private var now = 1_000L

    @Before
    fun setUp() {
        db = Room.inMemoryDatabaseBuilder(ApplicationProvider.getApplicationContext<Context>(), ClipDatabase::class.java)
            .setDriver(AndroidSQLiteDriver())
            .build()
        repo = ClipRepository(db, tmp.newFolder("clips")) { now }
    }

    @After
    fun tearDown() = db.close()

    private suspend fun received(clip: Int = 7, bytes: ByteArray = byteArrayOf(1, 2, 3)): Long {
        val t = repo.beginTransfer("AA:BB", clip)
        t.partFile.parentFile!!.mkdirs()
        t.partFile.writeBytes(bytes)
        repo.saveProgress(t, bytes.size.toLong())
        val r = repo.commitReceived(t, CtrlMessage.ClipEnded(clip, bytes.size.toLong(), 2500, 0x1234, 900), 500, 3000)
        return r.rowId
    }

    @Test
    fun discardedTransferLeavesNoRowOrFile() = runBlocking {
        val kept = received(clip = 7)
        val t = repo.beginTransfer("AA:BB", 7) // a new recording under a wrapped id
        t.partFile.writeBytes(ByteArray(500))
        repo.saveProgress(t, 500)

        repo.discardTransfer(t)

        assertFalse(t.partFile.exists())
        assertNull(repo.get(t.rowId))
        assertEquals("the received row is untouched", ClipState.RECEIVED, repo.get(kept)!!.state)
        // Only an unfinished row is ever deleted.
        repo.discardTransfer(repo.latest("AA:BB", 7)!!)
        assertEquals(ClipState.RECEIVED, repo.get(kept)!!.state)
    }

    @Test
    fun transferResumesThenCommitsAndMovesTheFile() = runBlocking {
        val first = repo.beginTransfer("AA:BB", 7)
        repo.saveProgress(first, 4096)
        val again = repo.beginTransfer("AA:BB", 7)
        assertEquals("unfinished row is reused", first.rowId, again.rowId)
        assertEquals(4096, again.durableBytes)

        again.partFile.parentFile!!.mkdirs()
        again.partFile.writeBytes(ByteArray(10))
        val clip = repo.commitReceived(again, CtrlMessage.ClipEnded(7, 10, 2500, 0xCAFEBABE, 900), 500, 3000)
        assertFalse(again.partFile.exists())
        assertTrue(clip.file.exists())

        val rec = repo.get(clip.rowId)!!
        assertEquals(ClipState.RECEIVED, rec.state)
        assertEquals(0xCAFEBABE, rec.crc32)
        assertEquals(900L, rec.voicedMs)
        assertEquals(2500L, rec.durationMs)
        assertEquals(clip.file.path, rec.filePath)

        val latest = repo.latest("AA:BB", 7)!!
        assertTrue(latest.received)
        assertNotEquals("a received id starts a new row", clip.rowId, repo.beginTransfer("AA:BB", 7).rowId)
    }

    @Test
    fun promptIsClaimedOnce() = runBlocking {
        val id = received()
        assertFalse("not transcribed yet", repo.claimPrompt(id))
        repo.saveTranscript(id, "hello")
        val claims = (1..8).map { async { repo.claimPrompt(id) } }.awaitAll()
        assertEquals(1, claims.count { it })
        assertEquals(ClipState.PROMPTED, repo.get(id)!!.state)

        assertFalse(repo.transition(id, ClipState.TRANSCRIBED, ClipState.FAILED))
        assertTrue(repo.transition(id, ClipState.PROMPTED, ClipState.UNCERTAIN, "lost"))
        assertEquals("lost", repo.get(id)!!.error)
    }

    @Test
    fun resendNeedsAResendableStateAndTheFile() = runBlocking {
        val id = received()
        assertFalse("RECEIVED is not resendable", repo.prepareResend(id))

        repo.setState(id, ClipState.NO_SPEECH)
        assertTrue(repo.prepareResend(id))
        val rec = repo.get(id)!!
        assertEquals(ClipState.RECEIVED, rec.state)
        assertTrue("NO_SPEECH resend skips the voiced gate", rec.forceSpeech)
        assertNull(rec.error)

        repo.saveTranscript(id, "hi")
        repo.setState(id, ClipState.UNCERTAIN, "lost")
        assertTrue(repo.prepareResend(id))
        assertEquals(ClipState.TRANSCRIBED, repo.get(id)!!.state)

        repo.setState(id, ClipState.FAILED)
        java.io.File(repo.get(id)!!.filePath!!).delete()
        assertFalse("no audio, no resend", repo.prepareResend(id))
    }

    @Test
    fun pendingProcessingListsUnfinishedClips() = runBlocking {
        val a = received(1)
        val b = received(2)
        val c = received(3)
        repo.setState(b, ClipState.REPLIED)
        repo.saveTranscript(c, "x")
        repo.claimPrompt(c)
        repo.beginTransfer("AA:BB", 4)
        assertEquals(setOf(a, c), repo.pendingProcessing().toSet())
    }
}
