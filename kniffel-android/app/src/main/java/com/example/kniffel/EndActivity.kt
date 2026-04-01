package com.example.kniffel

import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import com.example.kniffel.MainActivity.Companion.PLAYER_COLORS
import com.example.kniffel.MainActivity.Companion.PLAYER_EMOJIS
import com.example.kniffel.model.GameState
import com.google.android.material.button.MaterialButton

class EndActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_end)

        val container = findViewById<LinearLayout>(R.id.endResultsContainer)
        val medals = listOf("🥇", "🥈", "🥉", "4️⃣", "5️⃣")

        val sorted = GameState.players.sortedByDescending { GameState.calcTotalScore(it) }

        sorted.forEachIndexed { rank, player ->
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(dpToPx(16), dpToPx(14), dpToPx(16), dpToPx(14))
                if (rank < sorted.size - 1) {
                    // Divider via background not possible here, add separator below
                }
            }

            val tvMedal = TextView(this).apply {
                text = medals[rank]
                textSize = 22f
                setPadding(0, 0, dpToPx(12), 0)
            }

            val tvName = TextView(this).apply {
                text = "${PLAYER_EMOJIS[player.index]}  ${player.name}"
                textSize = 16f
                setTextColor(Color.parseColor(PLAYER_COLORS[player.index]))
                layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
            }

            val tvScore = TextView(this).apply {
                text = "${GameState.calcTotalScore(player)}"
                textSize = 20f
                setTextColor(Color.parseColor("#F39C12"))
                typeface = Typeface.DEFAULT_BOLD
            }

            row.addView(tvMedal)
            row.addView(tvName)
            row.addView(tvScore)
            container.addView(row)

            if (rank < sorted.size - 1) {
                val div = android.view.View(this).apply {
                    setBackgroundColor(Color.parseColor("#11FFFFFF"))
                    layoutParams = LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT, dpToPx(1)
                    )
                }
                container.addView(div)
            }
        }

        findViewById<MaterialButton>(R.id.btnNewGame).setOnClickListener {
            startActivity(Intent(this, MainActivity::class.java).apply {
                flags = Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_NEW_TASK
            })
            finish()
        }
    }

    private fun dpToPx(dp: Int): Int = (dp * resources.displayMetrics.density).toInt()
}
