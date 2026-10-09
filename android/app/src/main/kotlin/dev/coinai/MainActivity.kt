package dev.coinai

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import dev.coinai.sambot.SambotClient
import dev.coinai.settings.SambotSettings
import dev.coinai.settings.SambotSettingsStore
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient
import java.io.IOException
import java.util.concurrent.TimeUnit

/** One OkHttp client (connection pool, dispatcher) for the whole app. */
object Http {
    val client: OkHttpClient by lazy {
        OkHttpClient.Builder()
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(60, TimeUnit.SECONDS)
            .build()
    }
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val store = SambotSettingsStore(this)
        setContent {
            CoinaiTheme {
                MainScreen(store)
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
private fun MainScreen(store: SambotSettingsStore) {
    Scaffold(topBar = { TopAppBar(title = { Text("coinai") }) }) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            StatusCard()
            SambotSection(store)
        }
    }
}

@Composable
private fun StatusCard() {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text("Status", style = MaterialTheme.typography.titleMedium)
            Text("No coin paired. Pairing, battery, last sync and pending clips arrive with Bluetooth support.")
        }
    }
}

private sealed interface TestState {
    data object Idle : TestState
    data object Running : TestState
    data class Done(val message: String, val ok: Boolean) : TestState
}

@Composable
private fun SambotSection(store: SambotSettingsStore) {
    val scope = rememberCoroutineScope()
    var loaded by rememberSaveable { mutableStateOf(false) }
    var baseUrl by rememberSaveable { mutableStateOf("") }
    var token by rememberSaveable { mutableStateOf("") }
    var savedNote by remember { mutableStateOf<String?>(null) }
    var test by remember { mutableStateOf<TestState>(TestState.Idle) }

    LaunchedEffect(Unit) {
        if (!loaded) {
            val stored = store.settings.first()
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
                            store.save(SambotSettings(baseUrl, token))
                            savedNote = "Saved"
                        }
                    },
                ) { Text("Save") }
                OutlinedButton(
                    enabled = loaded && test != TestState.Running,
                    onClick = {
                        scope.launch {
                            test = TestState.Running
                            test = testConnection(baseUrl, token)
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

private suspend fun testConnection(baseUrl: String, token: String): TestState.Done = try {
    val client = SambotClient(baseUrl, token, Http.client)
    if (client.health()) TestState.Done("Connected: /health answered ok", ok = true)
    else TestState.Done("/health answered, but not ok", ok = false)
} catch (e: IllegalArgumentException) {
    TestState.Done("Invalid URL: ${e.message}", ok = false)
} catch (e: IOException) {
    TestState.Done("Failed: ${e.message ?: e.javaClass.simpleName}", ok = false)
}
