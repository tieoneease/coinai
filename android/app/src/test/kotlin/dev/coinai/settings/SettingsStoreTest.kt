package dev.coinai.settings

import androidx.datastore.core.DataStoreFactory
import androidx.datastore.core.okio.OkioStorage
import androidx.datastore.preferences.core.PreferencesSerializer
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import dev.coinai.protocol.CtrlMessage
import dev.coinai.protocol.ErrorCode
import dev.coinai.sync.DeliveryOrder
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import okio.FileSystem
import okio.Path.Companion.toOkioPath
import java.io.File
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.spec.GCMParameterSpec

/**
 * The same AES-GCM as [KeystoreSecretBox] with an in-memory key: the Android Keystore itself does
 * not exist on the JVM (nor in Robolectric), so the keystore class is only exercised on a device.
 */
private class JvmSecretBox : SecretBox {
    val key = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
    var seals = 0

    override fun seal(plain: ByteArray): Sealed {
        seals++
        val iv = ByteArray(12).also { SecureRandom().nextBytes(it) }
        val c = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key, GCMParameterSpec(128, iv)) }
        return Sealed(c.doFinal(plain), iv)
    }

    override fun open(sealed: Sealed): ByteArray {
        val c = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, sealed.iv)) }
        return c.doFinal(sealed.ciphertext)
    }
}

class SettingsStoreTest {
    @get:Rule val tmp = TemporaryFolder()
    private val scopes = mutableListOf<CoroutineScope>()

    @After
    fun tearDown() = scopes.forEach { it.cancel() }

    // Okio storage: its atomic move replaces the target on Windows, where the File storage that
    // PreferenceDataStoreFactory uses on JVM cannot rename over an existing file. Same format.
    private fun dataStore(file: File, box: SecretBox? = null) = DataStoreFactory.create(
        storage = OkioStorage(FileSystem.SYSTEM, PreferencesSerializer) { file.toOkioPath() },
        migrations = listOfNotNull(box?.let { LegacyTokenMigration(it) }),
        scope = CoroutineScope(SupervisorJob() + Dispatchers.IO).also { scopes += it },
    )

    private fun rawText(file: File) = String(file.readBytes(), Charsets.ISO_8859_1)

    @Test
    fun tokenIsStoredOnlyAsCiphertext() = runBlocking {
        val file = File(tmp.root, "settings.preferences_pb")
        val box = JvmSecretBox()
        val store = SettingsStore(dataStore(file), box)
        store.saveSambot(SambotSettings(" https://sambot.example ", " secret-token-123 "))

        assertEquals(SambotSettings("https://sambot.example", "secret-token-123"), store.sambot.first())
        assertFalse("plain token on disk", rawText(file).contains("secret-token-123"))

        store.saveSambot(SambotSettings("https://sambot.example", ""))
        assertEquals("", store.sambot.first().token)
        assertEquals(1, box.seals)
    }

    @Test
    fun legacyPlainTokenIsEncryptedAndRemoved() = runBlocking {
        val file = File(tmp.root, "settings.preferences_pb")
        // What the first app version wrote.
        val old = dataStore(file)
        old.edit {
            it[stringPreferencesKey("sambot_base_url")] = "https://old.example"
            it[stringPreferencesKey("sambot_token")] = "legacy-token-456"
        }
        scopes.removeAt(0).cancel()

        val box = JvmSecretBox()
        val store = SettingsStore(dataStore(file, box), box)
        assertEquals(SambotSettings("https://old.example", "legacy-token-456"), store.sambot.first())
        assertFalse("plain token left on disk", rawText(file).contains("legacy-token-456"))
        assertTrue(rawText(file).contains("sambot_token_ct"))
    }

    @Test
    fun undecryptableTokenReadsAsEmpty() = runBlocking {
        val file = File(tmp.root, "settings.preferences_pb")
        SettingsStore(dataStore(file), JvmSecretBox()).saveSambot(SambotSettings("https://a.example", "t"))
        scopes.removeAt(0).cancel()

        // A different key, as after restoring the app data onto a new phone.
        val other = JvmSecretBox()
        val store = SettingsStore(dataStore(file), other)
        assertEquals(SambotSettings("https://a.example", ""), store.sambot.first())
    }

    @Test
    fun deliveryDefaultsToWifiFirst() = runBlocking {
        val store = SettingsStore(dataStore(File(tmp.root, "d.preferences_pb")), JvmSecretBox())
        assertEquals(DeliveryOrder.WIFI_FIRST, store.delivery.first())
        store.saveDelivery(DeliveryOrder.PHONE_ONLY)
        assertEquals(DeliveryOrder.PHONE_ONLY, store.delivery.first())
    }

    @Test
    fun wifiPasswordsAreStoredOnlyAsCiphertext() = runBlocking {
        val file = File(tmp.root, "w.preferences_pb")
        val token = JvmSecretBox()
        val wifi = JvmSecretBox()
        var now = 1000L
        val store = SettingsStore(dataStore(file), token, wifi, clock = { now++ })
        store.addWifiNetwork("home-net", "fake-wifi-pass-1")
        store.addWifiNetwork("open-cafe", "")

        assertFalse("plain password on disk", rawText(file).contains("fake-wifi-pass-1"))
        assertEquals(1, wifi.seals) // the open network has no password to seal
        assertEquals(0, token.seals)
        val pending = store.pendingNetworks("C0:A1")
        assertEquals(listOf("home-net" to "fake-wifi-pass-1", "open-cafe" to ""), pending.map { it.ssid to it.password })
        assertEquals(listOf(false, true), store.wifiNetworks.first().map { it.open })
    }

    @Test
    fun invalidNetworksAreNotAdded() = runBlocking {
        val store = SettingsStore(dataStore(File(tmp.root, "i.preferences_pb")), JvmSecretBox())
        for ((ssid, pass) in listOf("" to "", "s".repeat(33) to "", "home" to "p".repeat(7), "home" to "p".repeat(65))) {
            assertThrows(IllegalArgumentException::class.java) { runBlocking { store.addWifiNetwork(ssid, pass) } }
        }
        store.addWifiNetwork("s".repeat(32), "p".repeat(64))
        store.addWifiNetwork("x", "p".repeat(8))
        assertEquals(2, store.wifiNetworks.first().size)
    }

    @Test
    fun networkStatesPerCoinReplaceAndRemove() = runBlocking {
        var now = 1000L
        val store = SettingsStore(dataStore(File(tmp.root, "n.preferences_pb")), JvmSecretBox(), clock = { now++ })
        store.addWifiNetwork("home", "fake-pass-1")
        store.addWifiNetwork("guest", "")
        val (home, guest) = store.pendingNetworks("A")

        store.networkResult("A", home, null)
        store.networkResult("A", guest, ErrorCode.INVALID_VALUE)
        val infos = store.wifiNetworks.first().associateBy { it.ssid }
        assertEquals(WifiNetworkState.SENT, infos.getValue("home").state)
        assertEquals(WifiNetworkState.REFUSED, infos.getValue("guest").state)
        assertEquals(ErrorCode.INVALID_VALUE, infos.getValue("guest").errorCode)
        assertTrue("nothing left for coin A", store.pendingNetworks("A").isEmpty())
        assertEquals(2, store.pendingNetworks("B").size)

        // The same SSID again replaces the entry, newest last, waiting again; a result for the
        // replaced version is ignored.
        store.addWifiNetwork("home", "fake-pass-2")
        store.networkResult("A", home, ErrorCode.INVALID_VALUE)
        val list = store.wifiNetworks.first()
        assertEquals(listOf("guest", "home"), list.map { it.ssid })
        assertEquals(WifiNetworkState.WAITING, list.last().state)
        assertEquals(listOf("fake-pass-2"), store.pendingNetworks("A").map { it.password })

        store.removeWifiNetwork(list.first().id)
        assertEquals(listOf("home"), store.wifiNetworks.first().map { it.ssid })
    }

    @Test
    fun undecryptableWifiPasswordIsFlaggedAndNotSent() = runBlocking {
        val file = File(tmp.root, "u.preferences_pb")
        SettingsStore(dataStore(file), JvmSecretBox()).addWifiNetwork("home", "fake-pass-1")
        scopes.removeAt(0).cancel()

        val store = SettingsStore(dataStore(file), JvmSecretBox())
        assertFalse(store.wifiNetworks.first().single().passwordReadable)
        assertTrue(store.pendingNetworks("A").isEmpty())
    }

    @Test
    fun thresholdAndStatusRoundTrip() = runBlocking {
        val store = SettingsStore(dataStore(File(tmp.root, "s.preferences_pb")), JvmSecretBox())
        assertEquals(400L, store.voicedThresholdMs.first())
        assertNull(store.deviceStatus.first())

        store.saveVoicedThreshold(750)
        store.saveDeviceStatus("AA:BB", CtrlMessage.Status(255, true, true, false, 3, "0.2.0"), 42)
        assertEquals(750L, store.voicedThresholdMs.first())
        val s = store.deviceStatus.first()!!
        assertEquals(DeviceStatus("AA:BB", 255, true, true, false, 3, "0.2.0", 42), s)
        assertFalse(s.batteryKnown)
    }
}
