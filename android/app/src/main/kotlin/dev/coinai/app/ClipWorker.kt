package dev.coinai.app

import android.content.Context
import androidx.work.BackoffPolicy
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingWorkPolicy
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.OutOfQuotaPolicy
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import androidx.work.workDataOf
import dev.coinai.clips.ClipProcessor
import java.util.concurrent.TimeUnit

/** Runs [ClipProcessor.process] for one clip when the network is up; WorkManager retries it. */
class ClipWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        val id = inputData.getLong(KEY_CLIP, -1)
        if (id < 0) return Result.failure()
        val app = applicationContext as CoinaiApp
        return when (val outcome = app.processor.process(id, runAttemptCount)) {
            ClipProcessor.Outcome.Done -> Result.success()
            is ClipProcessor.Outcome.Retry -> {
                app.log("clip row $id: retry (${outcome.reason})")
                Result.retry()
            }
        }
    }

    companion object {
        private const val KEY_CLIP = "clip_row"

        /**
         * Unique work per clip. [replace] is for a manual resend; otherwise an already queued or
         * running job for the clip is kept, so a clip is never processed twice at once.
         */
        fun enqueue(context: Context, clipRowId: Long, replace: Boolean = false) {
            val request = OneTimeWorkRequestBuilder<ClipWorker>()
                .setInputData(workDataOf(KEY_CLIP to clipRowId))
                .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build())
                .setExpedited(OutOfQuotaPolicy.RUN_AS_NON_EXPEDITED_WORK_REQUEST)
                .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 30, TimeUnit.SECONDS)
                .build()
            WorkManager.getInstance(context).enqueueUniqueWork(
                "clip-$clipRowId",
                if (replace) ExistingWorkPolicy.REPLACE else ExistingWorkPolicy.KEEP,
                request,
            )
        }
    }
}
