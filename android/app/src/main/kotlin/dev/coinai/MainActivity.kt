package dev.coinai

import android.Manifest
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.text.format.DateUtils
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import dev.coinai.app.CoinaiApp
import dev.coinai.ble.CoinPairing
import dev.coinai.clips.ClipRecord
import dev.coinai.clips.ClipState
import dev.coinai.clips.PlainText
import dev.coinai.clips.ProcessingSettings
import dev.coinai.sambot.SambotClient
import dev.coinai.settings.DeviceStatus
import dev.coinai.settings.SambotSettings
import dev.coinai.settings.WifiNetworkInfo
import dev.coinai.settings.WifiNetworkState
import dev.coinai.sync.CoinWifi
import dev.coinai.sync.DeliveryOrder
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import java.io.File
import java.io.IOException

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val app = application as CoinaiApp
        setContent {
            CoinaiTheme {
                MainScreen(app)
            }
        }
    }
}

@Composable
private fun CoinaiTheme(content: @Composable () -> Unit) {
    val context = LocalContext.current
    // minSdk 36, so dynamic color is always available.
    val colors = if (isSystemInDarkTheme()) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
    MaterialTheme(colorScheme = colors, content = content)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun MainScreen(app: CoinaiApp) {
    val clips by app.clips.clips.collectAsState(initial = emptyList())
    val player = rememberPlayer()
    Scaffold(topBar = { TopAppBar(title = { Text("coinai") }) }) { padding ->
        LazyColumn(
            contentPadding = PaddingValues(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
            modifier = Modifier.padding(padding),
        ) {
            item { CoinCard(app) }
            item { Text("Clips", style = MaterialTheme.typography.titleMedium) }
            if (clips.isEmpty()) item { Text("No clips yet.", color = MaterialTheme.colorScheme.onSurfaceVariant) }
            items(clips, key = { it.id }) { clip -> ClipRow(app, clip, player) }
            item { SambotSection(app) }
            item { DeliverySection(app) }
            item { CoinWifiSection(app) }
            item { ProcessingSection(app) }
        }
    }
}

/** One ExoPlayer for the screen, playing at most one clip. */
private class ClipPlayer(val exo: ExoPlayer) {
    var playingId by mutableLongStateOf(-1L)

    fun toggle(clip: ClipRecord) {
        if (playingId == clip.id) {
            exo.stop()
            playingId = -1
            return
        }
        val path = clip.filePath ?: return
        exo.setMediaItem(MediaItem.fromUri(Uri.fromFile(File(path))))
        exo.prepare()
        exo.play()
        playingId = clip.id
    }
}

@Composable
private fun rememberPlayer(): ClipPlayer {
    val context = LocalContext.current
    val player = remember { ClipPlayer(ExoPlayer.Builder(context).build()) }
    DisposableEffect(player) {
        val listener = object : Player.Listener {
            override fun onPlaybackStateChanged(playbackState: Int) {
                if (playbackState == Player.STATE_ENDED || playbackState == Player.STATE_IDLE) player.playingId = -1
            }
        }
        player.exo.addListener(listener)
        onDispose {
            player.exo.removeListener(listener)
            player.exo.release()
        }
    }
    return player
}

@Composable
private fun CoinCard(app: CoinaiApp) {
    val context = LocalContext.current
    val status by app.settings.deviceStatus.collectAsState(initial = null)
    var associations by remember { mutableIntStateOf(CoinPairing.associations(context).size) }
    var pairNote by remember { mutableStateOf<String?>(null) }

    val chooser = rememberLauncherForActivityResult(ActivityResultContracts.StartIntentSenderForResult()) { result ->
        if (result.resultCode != android.app.Activity.RESULT_OK) pairNote = "Pairing cancelled"
    }
    val startPairing = {
        pairNote = "Looking for coins…"
        CoinPairing.associate(
            context,
            context.mainExecutor,
            launch = { chooser.launch(IntentSenderRequest.Builder(it).build()) },
            onAssociated = {
                associations = CoinPairing.associations(context).size
                pairNote = "Paired. Enter the code the coin shows when Android asks for it."
            },
            onError = { pairNote = "Pairing failed: $it" },
        )
    }
    val permissions = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
        if (granted[Manifest.permission.BLUETOOTH_CONNECT] == true) startPairing()
        else pairNote = "Pairing needs the Nearby devices permission"
    }

    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Coin", style = MaterialTheme.typography.titleMedium)
            val s = status
            if (s == null) {
                Text(if (associations == 0) "No coin paired." else "Paired. Waiting for the coin's first sync.")
            } else {
                StatusLines(s)
            }
            Button(
                enabled = CoinPairing.isSupported(context),
                onClick = {
                    val needed = listOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.POST_NOTIFICATIONS)
                        .filter { context.checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
                    if (needed.isEmpty()) startPairing() else permissions.launch(needed.toTypedArray())
                },
            ) { Text(if (associations == 0) "Pair coin" else "Pair another coin") }
            pairNote?.let { Text(it, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite }) }
        }
    }
}

@Composable
private fun StatusLines(s: DeviceStatus) {
    val battery = if (s.batteryKnown) "${s.batteryPct}%" else "unknown"
    val power = when {
        s.charging -> ", charging"
        s.usbPower -> ", on USB"
        else -> ""
    }
    Text("Battery $battery$power")
    Text(if (s.pending == 1) "1 clip waiting on the coin" else "${s.pending} clips waiting on the coin")
    if (s.recording) Text("Recording")
    Text(
        "Last sync ${DateUtils.getRelativeTimeSpanString(s.atMs)} · firmware ${s.fwVersion}",
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

private fun ClipState.label(): String = when (this) {
    ClipState.RECEIVING -> "Receiving"
    ClipState.RECEIVED -> "Received"
    ClipState.TRANSCRIBED -> "Transcribed"
    ClipState.PROMPTED -> "Sent to SamBot"
    ClipState.REPLIED -> "Replied"
    ClipState.NO_SPEECH -> "No speech"
    ClipState.UNCERTAIN -> "Unknown if SamBot got it"
    ClipState.FAILED -> "Failed"
}

@Composable
private fun ClipRow(app: CoinaiApp, clip: ClipRecord, player: ClipPlayer) {
    val scope = rememberCoroutineScope()
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "#${clip.clipId} · ${clip.state.label()}",
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.weight(1f),
                )
                clip.durationMs?.let { Text("%.1f s".format(it / 1000.0), style = MaterialTheme.typography.bodySmall) }
            }
            (clip.startedAtMs ?: clip.receivedAtMs)?.let {
                Text(
                    DateUtils.getRelativeTimeSpanString(it).toString(),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            clip.transcript?.takeIf { it.isNotBlank() }?.let { Text("“$it”") }
            clip.reply?.let { Text(PlainText.fromMarkdown(it), maxLines = 6) }
            clip.error?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                if (clip.filePath != null) {
                    TextButton(onClick = { player.toggle(clip) }) { Text(if (player.playingId == clip.id) "Stop" else "Play") }
                }
                if (clip.state.resendable) {
                    TextButton(onClick = { scope.launch { app.resend(clip.id) } }) { Text("Resend") }
                }
            }
        }
    }
}

private sealed interface TestState {
    data object Idle : TestState
    data object Running : TestState
    data class Done(val message: String, val ok: Boolean) : TestState
}

@Composable
private fun SambotSection(app: CoinaiApp) {
    val scope = rememberCoroutineScope()
    var loaded by rememberSaveable { mutableStateOf(false) }
    var baseUrl by rememberSaveable { mutableStateOf("") }
    var token by rememberSaveable { mutableStateOf("") }
    var savedNote by remember { mutableStateOf<String?>(null) }
    var test by remember { mutableStateOf<TestState>(TestState.Idle) }

    LaunchedEffect(Unit) {
        if (!loaded) {
            val stored = app.settings.sambot.first()
            baseUrl = stored.baseUrl
            token = stored.token
            loaded = true
        }
    }

    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("SamBot", style = MaterialTheme.typography.titleMedium)
            OutlinedTextField(
                value = baseUrl,
                onValueChange = { baseUrl = it; savedNote = null },
                label = { Text("Base URL") },
                singleLine = true,
                enabled = loaded,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = token,
                onValueChange = { token = it; savedNote = null },
                label = { Text("exe.dev VM token") },
                singleLine = true,
                enabled = loaded,
                visualTransformation = PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                modifier = Modifier.fillMaxWidth(),
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    enabled = loaded,
                    onClick = {
                        scope.launch {
                            app.settings.saveSambot(SambotSettings(baseUrl, token))
                            savedNote = "Saved"
                        }
                    },
                ) { Text("Save") }
                OutlinedButton(
                    enabled = loaded && test != TestState.Running,
                    onClick = {
                        scope.launch {
                            test = TestState.Running
                            test = testConnection(app, baseUrl, token)
                        }
                    },
                ) { Text("Test connection") }
            }
            val status = when (val t = test) {
                TestState.Idle -> savedNote
                TestState.Running -> "Testing…"
                is TestState.Done -> t.message
            }
            if (status != null) {
                Text(
                    text = status,
                    color = if ((test as? TestState.Done)?.ok == false) MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                )
            }
        }
    }
}

private fun DeliveryOrder.label(): String = when (this) {
    DeliveryOrder.WIFI_FIRST -> "Wi-Fi first"
    DeliveryOrder.PHONE_FIRST -> "Phone first"
    DeliveryOrder.PHONE_ONLY -> "Phone only"
}

private fun DeliveryOrder.description(): String = when (this) {
    DeliveryOrder.WIFI_FIRST -> "The coin sends clips to SamBot itself on a known network; this phone gets them when it can't."
    DeliveryOrder.PHONE_FIRST -> "Clips come to this phone when it's connected; the coin uses Wi-Fi only without it."
    DeliveryOrder.PHONE_ONLY -> "Clips always come to this phone."
}

/** PROTOCOL.md "Wi-Fi delivery": `CFG_SET` key 0x06, sent at every sync and at once during one. */
@Composable
private fun DeliverySection(app: CoinaiApp) {
    val scope = rememberCoroutineScope()
    val stored by app.settings.delivery.collectAsState(initial = null)
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Delivery", style = MaterialTheme.typography.titleMedium)
            Column(Modifier.selectableGroup()) {
                DeliveryOrder.entries.forEach { order ->
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier
                            .fillMaxWidth()
                            .selectable(
                                selected = stored == order,
                                enabled = stored != null,
                                role = Role.RadioButton,
                                onClick = { scope.launch { app.settings.saveDelivery(order) } },
                            )
                            .padding(vertical = 4.dp),
                    ) {
                        RadioButton(selected = stored == order, onClick = null, enabled = stored != null)
                        Column(modifier = Modifier.padding(start = 12.dp)) {
                            Text(order.label())
                            Text(
                                order.description(),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                }
            }
            Text(
                "Wi-Fi delivery needs a network below and the SamBot token on the coin, set over its USB console. " +
                    "The coin gets this setting at its next sync.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

private fun WifiNetworkInfo.stateLabel(): String = when {
    !passwordReadable -> "Password unreadable on this phone; remove and add again"
    state == WifiNetworkState.WAITING -> "Waiting for the coin's next sync"
    state == WifiNetworkState.SENT -> "Sent to the coin"
    else -> "The coin refused it (error ${errorCode ?: "?"})"
}

/** The coin's networks for Wi-Fi delivery (`WIFI_SET`), queued until the next sync. */
@Composable
private fun CoinWifiSection(app: CoinaiApp) {
    val scope = rememberCoroutineScope()
    val networks by app.settings.wifiNetworks.collectAsState(initial = emptyList())
    var ssid by rememberSaveable { mutableStateOf("") }
    // Not saveable: the password stays out of the saved instance state.
    var password by remember { mutableStateOf("") }
    var open by rememberSaveable { mutableStateOf(false) }
    var note by remember { mutableStateOf<String?>(null) }

    val ssidError = if (ssid.isEmpty()) null else CoinWifi.ssidError(ssid)
    val passwordError = when {
        open || password.isEmpty() -> null
        else -> CoinWifi.passwordError(password)
    }
    val canAdd = CoinWifi.ssidError(ssid) == null && (open || (password.isNotEmpty() && passwordError == null))

    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Coin Wi-Fi", style = MaterialTheme.typography.titleMedium)
            Text(
                "The coin keeps the ${CoinWifi.COIN_KEEPS} newest networks. It needs 2.4 GHz Wi-Fi without a sign-in page. " +
                    "Removing a network here does not remove it from the coin, which cannot forget one over Bluetooth yet; " +
                    "it drops the oldest when newer ones arrive.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            networks.asReversed().forEach { n ->
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(n.ssid + if (n.open) " (open)" else "")
                        Text(
                            n.stateLabel(),
                            style = MaterialTheme.typography.bodySmall,
                            color = if (n.state == WifiNetworkState.REFUSED || !n.passwordReadable) MaterialTheme.colorScheme.error
                            else MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    TextButton(onClick = { scope.launch { app.settings.removeWifiNetwork(n.id) } }) { Text("Remove") }
                }
            }
            OutlinedTextField(
                value = ssid,
                onValueChange = { ssid = it; note = null },
                label = { Text("Network name (SSID)") },
                isError = ssidError != null,
                supportingText = ssidError?.let { { Text(it) } },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.toggleable(value = open, role = Role.Checkbox, onValueChange = { open = it; note = null }),
            ) {
                Checkbox(checked = open, onCheckedChange = null)
                Text("Open network (no password)", modifier = Modifier.padding(start = 12.dp))
            }
            if (!open) {
                OutlinedTextField(
                    value = password,
                    onValueChange = { password = it; note = null },
                    label = { Text("Password") },
                    isError = passwordError != null,
                    supportingText = { Text(passwordError ?: "8 to 64 characters (non-ASCII ones count as 2 or more)") },
                    singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
            Button(
                enabled = canAdd,
                onClick = {
                    val name = ssid
                    val pass = if (open) "" else password
                    scope.launch {
                        note = try {
                            app.settings.addWifiNetwork(name, pass)
                            ssid = ""
                            password = ""
                            open = false
                            "Added. It goes to the coin at its next sync."
                        } catch (e: IllegalArgumentException) {
                            e.message
                        }
                    }
                },
            ) { Text("Add network") }
            note?.let { Text(it, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite }) }
        }
    }
}

@Composable
private fun ProcessingSection(app: CoinaiApp) {
    val scope = rememberCoroutineScope()
    val stored by app.settings.voicedThresholdMs.collectAsState(initial = null)
    var text by rememberSaveable { mutableStateOf<String?>(null) }
    LaunchedEffect(stored) { if (text == null && stored != null) text = stored.toString() }
    val value = text?.toLongOrNull()

    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("Processing", style = MaterialTheme.typography.titleMedium)
            OutlinedTextField(
                value = text.orEmpty(),
                onValueChange = { text = it.filter(Char::isDigit).take(6) },
                label = { Text("Minimum speech (ms)") },
                supportingText = {
                    Text("Clips with less voiced audio are not sent. Default ${ProcessingSettings.DEFAULT_VOICED_THRESHOLD_MS} ms.")
                },
                isError = text != null && value == null,
                singleLine = true,
                enabled = stored != null,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.fillMaxWidth(),
            )
            Button(
                enabled = value != null && value != stored,
                onClick = { value?.let { scope.launch { app.settings.saveVoicedThreshold(it) } } },
            ) { Text("Save") }
        }
    }
}

private suspend fun testConnection(app: CoinaiApp, baseUrl: String, token: String): TestState.Done = try {
    val client = SambotClient(baseUrl, token, app.http)
    if (client.health()) TestState.Done("Connected: /health answered ok", ok = true)
    else TestState.Done("/health answered, but not ok", ok = false)
} catch (e: IllegalArgumentException) {
    TestState.Done("Invalid URL: ${e.message}", ok = false)
} catch (e: IOException) {
    TestState.Done("Failed: ${e.message ?: e.javaClass.simpleName}", ok = false)
}
