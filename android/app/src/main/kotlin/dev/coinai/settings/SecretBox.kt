package dev.coinai.settings

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

class Sealed(val ciphertext: ByteArray, val iv: ByteArray)

/** Authenticated encryption of small secrets. */
interface SecretBox {
    fun seal(plain: ByteArray): Sealed

    /** Throws [java.security.GeneralSecurityException] when the data or the key is not valid. */
    fun open(sealed: Sealed): ByteArray
}

/**
 * AES-256-GCM with a non-exportable key in the Android Keystore. The keystore picks a fresh random
 * IV for each encryption; ciphertext and IV are stored side by side in DataStore. [aad] binds the
 * ciphertext to its purpose.
 */
class KeystoreSecretBox(private val alias: String, private val aad: ByteArray = alias.toByteArray()) : SecretBox {
    private fun key(): SecretKey {
        val keyStore = KeyStore.getInstance(KEYSTORE).apply { load(null) }
        (keyStore.getKey(alias, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
        generator.init(
            KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return generator.generateKey()
    }

    override fun seal(plain: ByteArray): Sealed {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, key())
        cipher.updateAAD(aad)
        return Sealed(cipher.doFinal(plain), cipher.iv)
    }

    override fun open(sealed: Sealed): ByteArray {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(TAG_BITS, sealed.iv))
        cipher.updateAAD(aad)
        return cipher.doFinal(sealed.ciphertext)
    }

    private companion object {
        const val KEYSTORE = "AndroidKeyStore"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val TAG_BITS = 128
    }
}
