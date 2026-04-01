package com.example.kniffel

import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import com.example.kniffel.model.GameState
import com.google.android.material.button.MaterialButton

class MainActivity : AppCompatActivity() {

    companion object {
        val PLAYER_COLORS = listOf(
            "#E74C3C", "#3498DB", "#2ECC71", "#F39C12", "#9B59B6"
        )
        val PLAYER_EMOJIS = listOf("🔴", "🔵", "🟢", "🟡", "🟣")
    }

    private var playerCount = 2
    private val nameFields = mutableListOf<EditText>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        buildCountButtons()
        buildPlayerInputs()

        findViewById<MaterialButton>(R.id.btnStart).setOnClickListener {
            startGame()
        }
    }

    private fun buildCountButtons() {
        val container = findViewById<LinearLayout>(R.id.countButtonContainer)
        container.removeAllViews()
        for (i in 1..5) {
            val btn = MaterialButton(this).apply {
                text = i.toString()
                isSelected = (i == playerCount)
                setBackgroundColor(
                    if (i == playerCount) Color.parseColor("#E74C3C")
                    else Color.parseColor("#2A3A50")
                )
                setTextColor(Color.WHITE)
                cornerRadius = 50
                setOnClickListener {
                    playerCount = i
                    buildCountButtons()
                    buildPlayerInputs()
                }
            }
            val params = LinearLayout.LayoutParams(0, dpToPx(48), 1f).apply {
                setMargins(dpToPx(4), 0, dpToPx(4), 0)
            }
            container.addView(btn, params)
        }
    }

    private fun buildPlayerInputs() {
        val container = findViewById<LinearLayout>(R.id.playerInputContainer)
        container.removeAllViews()
        nameFields.clear()

        for (i in 0 until playerCount) {
            val row = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }

            val dot = TextView(this).apply {
                text = PLAYER_EMOJIS[i]
                textSize = 22f
                setPadding(0, 0, dpToPx(12), 0)
            }

            val et = EditText(this).apply {
                hint = "Spieler ${i + 1}"
                setText("Spieler ${i + 1}")
                setTextColor(Color.WHITE)
                setHintTextColor(Color.parseColor("#88AABBCC"))
                background = null
                setPadding(dpToPx(12), dpToPx(10), dpToPx(12), dpToPx(10))
                layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
            }

            val divider = View(this).apply {
                setBackgroundColor(Color.parseColor("#33FFFFFF"))
            }

            row.addView(dot)
            row.addView(et)

            val rowWrapper = LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                background = getDrawable(R.drawable.bg_input_card)
                setPadding(dpToPx(14), dpToPx(4), dpToPx(14), dpToPx(4))
                val p = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply { setMargins(0, dpToPx(8), 0, 0) }
                layoutParams = p
            }
            rowWrapper.addView(row)
            container.addView(rowWrapper)
            nameFields.add(et)
        }
    }

    private fun startGame() {
        val names = nameFields.mapIndexed { i, et ->
            et.text.toString().ifBlank { "Spieler ${i + 1}" }
        }
        GameState.reset(names)
        startActivity(Intent(this, GameActivity::class.java))
    }

    private fun dpToPx(dp: Int): Int = (dp * resources.displayMetrics.density).toInt()
}
