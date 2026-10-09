package dev.coinai.settings

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import dev.coinai.sambot.SambotClient
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

data class SambotSettings(val baseUrl: String, val token: String)

private val Context.settingsDataStore: DataStore<Preferences> by preferencesDataStore(name = "settings")

/** SamBot connection settings in Jetpack DataStore. */
class SambotSettingsStore(context: Context) {
    private val store = context.applicationContext.settingsDataStore

    val settings: Flow<SambotSettings> = store.data.map {
        SambotSettings(
            baseUrl = it[BASE_URL] ?: SambotClient.DEFAULT_BASE_URL,
            token = it[TOKEN].orEmpty(),
        )
    }

    suspend fun save(settings: SambotSettings) {
        store.edit {
            it[BASE_URL] = settings.baseUrl.trim()
            // TODO(token-encryption): the exe.dev VM token is stored in plain text here. PLAN.md
            //  "Reaching SamBot" requires it stored encrypted on the phone (an Android Keystore key);
            //  that is a separate, later task.
            it[TOKEN] = settings.token.trim()
        }
    }

    private companion object {
        val BASE_URL = stringPreferencesKey("sambot_base_url")
        val TOKEN = stringPreferencesKey("sambot_token")
    }
}
