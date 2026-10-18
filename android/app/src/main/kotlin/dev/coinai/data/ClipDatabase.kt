package dev.coinai.data

import android.content.Context
import androidx.room3.ColumnInfo
import androidx.room3.Dao
import androidx.room3.Database
import androidx.room3.Entity
import androidx.room3.Index
import androidx.room3.Insert
import androidx.room3.PrimaryKey
import androidx.room3.Query
import androidx.room3.Room
import androidx.room3.RoomDatabase
import androidx.room3.migration.Migration
import androidx.sqlite.execSQL
import androidx.sqlite.driver.AndroidSQLiteDriver
import dev.coinai.clips.ClipRecord
import dev.coinai.clips.ClipState
import kotlinx.coroutines.flow.Flow

@Entity(tableName = "clips", indices = [Index(value = ["device", "clipId"])])
data class ClipEntity(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val device: String,
    val clipId: Int,
    val state: ClipState,
    val size: Long? = null,
    val crc32: Long? = null,
    val durationMs: Long? = null,
    val voicedMs: Long? = null,
    val receivedBytes: Long = 0,
    val startedAtMs: Long? = null,
    val receivedAtMs: Long? = null,
    val ackedAtMs: Long? = null,
    val filePath: String? = null,
    val transcript: String? = null,
    val reply: String? = null,
    @ColumnInfo(defaultValue = "0") val replyPartial: Boolean = false,
    val error: String? = null,
    @ColumnInfo(defaultValue = "0") val forceSpeech: Boolean = false,
    val createdAtMs: Long,
    /** Since version 2: the composer's scene and a `later` scene's hand-off text. */
    val scene: String? = null,
    val handoff: String? = null,
) {
    fun toRecord() = ClipRecord(
        id, device, clipId, state, size, crc32, durationMs, voicedMs, receivedBytes, startedAtMs, receivedAtMs,
        ackedAtMs, filePath, transcript, reply, replyPartial, scene, handoff, error, forceSpeech, createdAtMs,
    )
}

@Dao
interface ClipDao {
    @Insert
    suspend fun insert(clip: ClipEntity): Long

    @Query("SELECT * FROM clips WHERE id = :id")
    suspend fun get(id: Long): ClipEntity?

    @Query("SELECT * FROM clips WHERE device = :device AND clipId = :clipId ORDER BY id DESC LIMIT 1")
    suspend fun latest(device: String, clipId: Int): ClipEntity?

    @Query("SELECT clipId FROM clips WHERE device = :device AND state = 'RECEIVING' GROUP BY clipId ORDER BY MIN(id)")
    suspend fun receivingClipIds(device: String): List<Int>

    @Query("SELECT * FROM clips ORDER BY id DESC")
    fun observeAll(): Flow<List<ClipEntity>>

    @Query("SELECT * FROM clips WHERE state IN (:states)")
    suspend fun inStates(states: List<ClipState>): List<ClipEntity>

    @Query("UPDATE clips SET receivedBytes = :bytes WHERE id = :id AND state = 'RECEIVING'")
    suspend fun setProgress(id: Long, bytes: Long): Int

    @Query(
        """UPDATE clips SET state = 'RECEIVED', size = :size, crc32 = :crc32, durationMs = :durationMs,
           voicedMs = :voicedMs, receivedBytes = :size, startedAtMs = :startedAtMs, receivedAtMs = :receivedAtMs,
           filePath = :filePath, error = NULL WHERE id = :id AND state = 'RECEIVING'""",
    )
    suspend fun markReceived(
        id: Long, size: Long, crc32: Long, durationMs: Long, voicedMs: Long, startedAtMs: Long, receivedAtMs: Long, filePath: String,
    ): Int

    @Query("DELETE FROM clips WHERE id = :id AND state = 'RECEIVING'")
    suspend fun deleteReceiving(id: Long): Int

    @Query("UPDATE clips SET ackedAtMs = :atMs WHERE id = :id")
    suspend fun markAcked(id: Long, atMs: Long)

    /** Conditional transition: changes nothing (returns 0) unless the clip is in [from]. */
    @Query("UPDATE clips SET state = :to WHERE id = :id AND state = :from")
    suspend fun transition(id: Long, from: ClipState, to: ClipState): Int

    @Query("UPDATE clips SET state = :to, error = :error WHERE id = :id AND state = :from")
    suspend fun transitionWithError(id: Long, from: ClipState, to: ClipState, error: String?): Int

    @Query("UPDATE clips SET state = :state, error = :error WHERE id = :id")
    suspend fun setState(id: Long, state: ClipState, error: String?)

    @Query("UPDATE clips SET state = 'TRANSCRIBED', transcript = :transcript, error = NULL WHERE id = :id AND state = 'RECEIVED'")
    suspend fun saveTranscript(id: Long, transcript: String): Int

    @Query("UPDATE clips SET state = 'REPLIED', reply = :reply, replyPartial = :partial, error = NULL WHERE id = :id AND state = 'PROMPTED'")
    suspend fun saveReply(id: Long, reply: String, partial: Boolean): Int

    @Query(
        """UPDATE clips SET state = 'REPLIED', scene = :scene, reply = :summary, replyPartial = 0, error = NULL
           WHERE id = :id AND state = 'TRANSCRIBED'""",
    )
    suspend fun saveSceneReply(id: Long, scene: String, summary: String): Int

    @Query("UPDATE clips SET scene = :scene, handoff = :handoff WHERE id = :id AND state = 'TRANSCRIBED'")
    suspend fun saveHandoff(id: Long, scene: String, handoff: String): Int

    @Query("UPDATE clips SET state = :to, forceSpeech = :forceSpeech, error = NULL WHERE id = :id")
    suspend fun resetForResend(id: Long, to: ClipState, forceSpeech: Boolean)
}

@Database(entities = [ClipEntity::class], version = 2, exportSchema = false)
abstract class ClipDatabase : RoomDatabase() {
    abstract fun clips(): ClipDao

    companion object {
        /** Version 2 (scenes v1): nullable `scene` and `handoff` columns; existing rows get NULL. */
        val MIGRATION_1_2 = Migration(1, 2) { connection ->
            connection.execSQL("ALTER TABLE clips ADD COLUMN scene TEXT")
            connection.execSQL("ALTER TABLE clips ADD COLUMN handoff TEXT")
        }

        fun open(context: Context): ClipDatabase =
            Room.databaseBuilder(context.applicationContext, ClipDatabase::class.java, "clips.db")
                .setDriver(AndroidSQLiteDriver())
                .addMigrations(MIGRATION_1_2)
                .build()
    }
}
