package dev.coinai.settings

import android.content.Context
import androidx.datastore.core.DataMigration
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import dev.coinai.clips.ProcessingSettings
import dev.coinai.protocol.CtrlMessage
import dev.coinai.sambot.SambotClient
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import java.security.GeneralSecurityException
import java.util.Base64

data class SambotSettings(val baseUrl: String, val token: String)

/** The coin's last STATUS, kept so the UI can show it between syncs. */
data class DeviceStatus(
    val device: String,
    val batteryPct: Int,
    val charging: Boolean,
    val usbPower: Boolean,
    val recording: Boolean,
    val pending: Int,
    val fwVersion: String,
    val atMs: Long,
) {
    val batteryKnown: Boolean get() = batteryPct != dev.coinai.protocol.Protocol.BATTERY_UNKNOWN
}

/**
 * App settings in Jetpack DataStore. The SamBot token is stored only as AES-GCM ciphertext and IV
 * (key in the Android Keystore, see [KeystoreSecretBox]); a plain token from the first app version
 * is encrypted by [LegacyTokenMigration] on first read.
 */
class SettingsStore(private val store: DataStore<Preferences>, private val box: SecretBox) {
    val sambot: Flow<SambotSettings> = store.data.map {
        SambotSettings(baseUrl = it[BASE_URL] ?: SambotClient.DEFAULT_BASE_URL, token = decryptToken(it))
    }

    val voicedThresholdMs: Flow<Long> = store.data.map { it[VOICED_THRESHOLD_MS] ?: ProcessingSettings.DEFAULT_VOICED_THRESHOLD_MS }

    val deviceStatus: Flow<DeviceStatus?> = store.data.map { p ->
        val device = p[STATUS_DEVICE] ?: return@map null
        DeviceStatus(
            device, p[STATUS_BATTERY] ?: 255, p[STATUS_CHARGING] ?: false, p[STATUS_USB] ?: false, p[STATUS_RECORDING] ?: false,
            p[STATUS_PENDING] ?: 0, p[STATUS_FW].orEmpty(), p[STATUS_AT] ?: 0,
        )
    }

    suspend fun saveSambot(settings: SambotSettings) {
        val sealed = settings.token.trim().takeIf { it.isNotEmpty() }?.let { box.seal(it.toByteArray()) }
        store.edit {
            it[BASE_URL] = settings.baseUrl.trim()
            writeToken(it, sealed)
        }
    }

    suspend fun saveVoicedThreshold(ms: Long) {
        store.edit { it[VOICED_THRESHOLD_MS] = ms.coerceAtLeast(0) }
    }

    suspend fun saveDeviceStatus(device: String, status: CtrlMessage.Status, atMs: Long) {
        store.edit {
            it[STATUS_DEVICE] = device
            it[STATUS_BATTERY] = status.batteryPct
            it[STATUS_CHARGING] = status.charging
            it[STATUS_USB] = status.usbPower
            it[STATUS_RECORDING] = status.recording
            it[STATUS_PENDING] = status.pending
            it[STATUS_FW] = status.fwVersion
            it[STATUS_AT] = atMs
        }
    }

    private fun decryptToken(p: Preferences): String {
        val ct = p[TOKEN_CIPHERTEXT] ?: return ""
        val iv = p[TOKEN_IV] ?: return ""
        return try {
            String(box.open(Sealed(B64D.decode(ct), B64D.decode(iv))))
        } catch (_: GeneralSecurityException) {
            "" // key gone (for example after a restore to a new phone): the user enters it again
        } catch (_: IllegalArgumentException) {
            ""
        }
    }

    companion object {
        private val BASE_URL = stringPreferencesKey("sambot_base_url")
        /** First-version plain-text token; only read by the migration, then removed. */
        internal val LEGACY_TOKEN = stringPreferencesKey("sambot_token")
        internal val TOKEN_CIPHERTEXT = stringPreferencesKey("sambot_token_ct")
        internal val TOKEN_IV = stringPreferencesKey("sambot_token_iv")
        private val VOICED_THRESHOLD_MS = longPreferencesKey("voiced_threshold_ms")
        private val STATUS_DEVICE = stringPreferencesKey("status_device")
        private val STATUS_BATTERY = intPreferencesKey("status_battery")
        private val STATUS_CHARGING = booleanPreferencesKey("status_charging")
        private val STATUS_USB = booleanPreferencesKey("status_usb")
        private val STATUS_RECORDING = booleanPreferencesKey("status_recording")
        private val STATUS_PENDING = intPreferencesKey("status_pending")
        private val STATUS_FW = stringPreferencesKey("status_fw")
        private val STATUS_AT = longPreferencesKey("status_at")

        private val B64E = Base64.getEncoder()
        private val B64D = Base64.getDecoder()
        private const val KEY_ALIAS = "coinai.sambot_token"

        internal fun writeToken(p: androidx.datastore.preferences.core.MutablePreferences, sealed: Sealed?) {
            p.remove(LEGACY_TOKEN)
            if (sealed == null) {
                p.remove(TOKEN_CIPHERTEXT)
                p.remove(TOKEN_IV)
            } else {
                p[TOKEN_CIPHERTEXT] = B64E.encodeToString(sealed.ciphertext)
                p[TOKEN_IV] = B64E.encodeToString(sealed.iv)
            }
        }

        internal const val TOKEN_KEY_ALIAS = KEY_ALIAS

        fun create(context: Context) = SettingsStore(context.applicationContext.settingsDataStore, KeystoreSecretBox(KEY_ALIAS))
    }
}

private val Context.settingsDataStore: DataStore<Preferences> by preferencesDataStore(
    name = "settings",
    produceMigrations = { listOf(LegacyTokenMigration(KeystoreSecretBox(SettingsStore.TOKEN_KEY_ALIAS))) },
)

/** Encrypts the plain-text token stored by the first app version and deletes the plain copy. */
class LegacyTokenMigration(private val box: SecretBox) : DataMigration<Preferences> {
    override suspend fun shouldMigrate(currentData: Preferences): Boolean = currentData[SettingsStore.LEGACY_TOKEN] != null

    override suspend fun migrate(currentData: Preferences): Preferences {
        val plain = currentData[SettingsStore.LEGACY_TOKEN].orEmpty().trim()
        val p = currentData.toMutablePreferences()
        SettingsStore.writeToken(p, plain.takeIf { it.isNotEmpty() }?.let { box.seal(it.toByteArray()) })
        return p.toPreferences()
    }

    override suspend fun cleanUp() = Unit
}
