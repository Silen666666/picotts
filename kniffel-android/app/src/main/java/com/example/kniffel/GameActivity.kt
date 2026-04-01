package com.example.kniffel

import android.animation.ObjectAnimator
import android.content.Intent
import android.graphics.Color
import android.hardware.SensorManager
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.animation.OvershootInterpolator
import android.widget.*
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.example.kniffel.MainActivity.Companion.PLAYER_COLORS
import com.example.kniffel.MainActivity.Companion.PLAYER_EMOJIS
import com.example.kniffel.model.Categories
import com.example.kniffel.model.GameState
import com.google.android.material.button.MaterialButton

class GameActivity : AppCompatActivity() {

    private lateinit var sensorManager: SensorManager
    private lateinit var shakeDetector: ShakeDetector
    private val diceViews = mutableListOf<TextView>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_game)

        sensorManager = getSystemService(SENSOR_SERVICE) as SensorManager
        shakeDetector = ShakeDetector { runOnUiThread { rollDice() } }

        buildDice()

        findViewById<MaterialButton>(R.id.btnRoll).setOnClickListener {
            rollDice()
        }

        renderAll()
    }

    override fun onResume() {
        super.onResume()
        val accel = sensorManager.getDefaultSensor(android.hardware.Sensor.TYPE_ACCELEROMETER)
        if (accel != null) {
            sensorManager.registerListener(shakeDetector, accel, SensorManager.SENSOR_DELAY_UI)
        }
    }

    override fun onPause() {
        super.onPause()
        sensorManager.unregisterListener(shakeDetector)
    }

    // ===== DICE =====

    private fun buildDice() {
        val container = findViewById<LinearLayout>(R.id.diceContainer)
        container.removeAllViews()
        diceViews.clear()
        for (i in 0 until 5) {
            val tv = TextView(this).apply {
                text = diceFace(GameState.dice[i])
                textSize = 34f
                gravity = Gravity.CENTER
                setTextColor(Color.parseColor("#2C3E50"))
                background = getDrawable(R.drawable.bg_die)
                val size = dpToPx(60)
                layoutParams = LinearLayout.LayoutParams(size, size).apply {
                    setMargins(dpToPx(4), 0, dpToPx(4), 0)
                }
                setOnClickListener { toggleHold(i) }
            }
            container.addView(tv)
            diceViews.add(tv)
        }
    }

    private fun toggleHold(index: Int) {
        if (!GameState.hasRolled || GameState.rollsLeft == 0) return
        GameState.held[index] = !GameState.held[index]
        renderDice()
    }

    private fun rollDice() {
        val gs = GameState
        if (gs.rollsLeft <= 0) {
            Toast.makeText(this, "Bitte eine Kategorie auswählen!", Toast.LENGTH_SHORT).show()
            return
        }
        if (!gs.hasRolled) gs.held = BooleanArray(5)

        for (i in 0 until 5) {
            if (!gs.held[i]) gs.dice[i] = (1..6).random()
        }
        gs.rollsLeft--
        gs.hasRolled = true

        animateDice()
        renderAll()
    }

    private fun animateDice() {
        for (i in 0 until 5) {
            if (!GameState.held[i]) {
                val view = diceViews[i]
                view.animate().cancel()
                ObjectAnimator.ofFloat(view, "rotation", 0f, -20f, 15f, -8f, 0f).apply {
                    duration = 400
                    interpolator = OvershootInterpolator()
                    start()
                }
                ObjectAnimator.ofFloat(view, "scaleX", 1f, 1.15f, 1f).apply {
                    duration = 400
                    start()
                }
                ObjectAnimator.ofFloat(view, "scaleY", 1f, 1.15f, 1f).apply {
                    duration = 400
                    start()
                }
            }
        }
    }

    private fun renderDice() {
        for (i in 0 until 5) {
            val tv = diceViews[i]
            tv.text = diceFace(GameState.dice[i])
            if (GameState.held[i]) {
                tv.background = getDrawable(R.drawable.bg_die_held)
                tv.animate().translationY(dpToPx(-8).toFloat()).setDuration(150).start()
            } else {
                tv.background = getDrawable(R.drawable.bg_die)
                tv.animate().translationY(0f).setDuration(150).start()
            }
        }
    }

    private fun diceFace(v: Int) = arrayOf("⚀", "⚁", "⚂", "⚃", "⚄", "⚅")[v - 1]

    // ===== RENDER =====

    private fun renderAll() {
        renderHeader()
        renderDice()
        renderRollButton()
        renderScoreSheet()
    }

    private fun renderHeader() {
        val gs = GameState
        val p = gs.currentPlayerObj()
        val color = Color.parseColor(PLAYER_COLORS[p.index])

        findViewById<TextView>(R.id.tvPlayerEmoji).text = PLAYER_EMOJIS[p.index]
        val tvName = findViewById<TextView>(R.id.tvPlayerName)
        tvName.text = "${p.name} ist dran"
        tvName.setTextColor(color)

        findViewById<TextView>(R.id.tvRound).text = "Runde ${gs.round} / 13"

        // Rolls dots
        val dotsContainer = findViewById<LinearLayout>(R.id.rollsDotsContainer)
        dotsContainer.removeAllViews()
        for (i in 0 until 3) {
            val dot = View(this).apply {
                val s = dpToPx(12)
                layoutParams = LinearLayout.LayoutParams(s, s).apply { setMargins(dpToPx(3), 0, dpToPx(3), 0) }
                setBackgroundResource(
                    if (i < gs.rollsLeft) R.drawable.dot_active else R.drawable.dot_inactive
                )
            }
            dotsContainer.addView(dot)
        }

        // Score tabs
        val tabContainer = findViewById<LinearLayout>(R.id.tabContainer)
        tabContainer.removeAllViews()
        gs.players.forEach { player ->
            val tab = TextView(this).apply {
                text = "${player.name}\n${gs.calcTotalScore(player)}"
                textSize = 11f
                gravity = Gravity.CENTER
                setTextColor(
                    if (player.index == gs.currentPlayer) Color.WHITE
                    else Color.parseColor("#88FFFFFF")
                )
                setPadding(dpToPx(8), dpToPx(8), dpToPx(8), dpToPx(8))
                layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
                if (player.index == gs.currentPlayer) {
                    setBackgroundResource(R.drawable.bg_tab_active)
                }
            }
            tabContainer.addView(tab)
        }
    }

    private fun renderRollButton() {
        val btn = findViewById<MaterialButton>(R.id.btnRoll)
        val remaining = GameState.rollsLeft
        btn.isEnabled = remaining > 0
        btn.text = when {
            !GameState.hasRolled -> "🎲  WÜRFELN  (oder schütteln)"
            remaining > 0 -> "🎲  NOCHMAL WÜRFELN  ($remaining übrig)"
            else -> "Kategorie auswählen ↓"
        }
    }

    // ===== SCORE SHEET =====

    private fun renderScoreSheet() {
        val gs = GameState
        val player = gs.currentPlayerObj()
        val container = findViewById<LinearLayout>(R.id.scoreContainer)
        container.removeAllViews()

        addSectionHeader(container, "OBERE SEKTION", upperBonusText(player))
        Categories.all.filter { it.section == "upper" }.forEach { cat ->
            addCategoryRow(container, cat, player)
        }

        addSectionHeader(container, "UNTERE SEKTION", "")
        Categories.all.filter { it.section == "lower" }.forEach { cat ->
            addCategoryRow(container, cat, player)
        }

        // Total
        val totalRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.parseColor("#33E74C3C"))
            setPadding(dpToPx(16), dpToPx(14), dpToPx(16), dpToPx(14))
        }
        val totalLabel = TextView(this).apply {
            text = "GESAMTPUNKTZAHL"
            textSize = 13f
            setTextColor(Color.WHITE)
            typeface = android.graphics.Typeface.DEFAULT_BOLD
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }
        val totalVal = TextView(this).apply {
            text = "${gs.calcTotalScore(player)}"
            textSize = 18f
            setTextColor(Color.parseColor("#E74C3C"))
            typeface = android.graphics.Typeface.DEFAULT_BOLD
        }
        totalRow.addView(totalLabel)
        totalRow.addView(totalVal)
        container.addView(totalRow)
    }

    private fun upperBonusText(player: GameState.Player): String {
        val upper = GameState.calcUpperScore(player)
        val need = 63 - upper
        return if (need > 0) "Bonus: noch $need Pkt." else "+35 Bonus ✓"
    }

    private fun addSectionHeader(container: LinearLayout, title: String, extra: String) {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setBackgroundColor(Color.parseColor("#22FFFFFF"))
            setPadding(dpToPx(14), dpToPx(8), dpToPx(14), dpToPx(8))
        }
        val tv = TextView(this).apply {
            text = title
            textSize = 10f
            letterSpacing = 0.15f
            setTextColor(Color.parseColor("#99FFFFFF"))
            typeface = android.graphics.Typeface.DEFAULT_BOLD
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }
        val tvExtra = TextView(this).apply {
            text = extra
            textSize = 11f
            setTextColor(
                if (extra.contains("✓")) Color.parseColor("#2ECC71")
                else Color.parseColor("#F39C12")
            )
        }
        row.addView(tv)
        row.addView(tvExtra)
        container.addView(row)
    }

    private fun addCategoryRow(container: LinearLayout, cat: com.example.kniffel.model.Category, player: GameState.Player) {
        val gs = GameState
        val scored = player.scores[cat.id]
        val isScored = player.scores.containsKey(cat.id)

        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dpToPx(14), dpToPx(11), dpToPx(14), dpToPx(11))
        }

        val nameCol = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
        }
        val tvName = TextView(this).apply {
            text = cat.name
            textSize = 14f
            setTextColor(Color.WHITE)
        }
        val tvHint = TextView(this).apply {
            text = cat.hint
            textSize = 10f
            setTextColor(Color.parseColor("#66FFFFFF"))
        }
        nameCol.addView(tvName)
        nameCol.addView(tvHint)
        row.addView(nameCol)

        when {
            isScored && scored != null -> {
                // Already scored
                tvName.alpha = 0.7f
                tvHint.alpha = 0.7f
                val tvVal = TextView(this).apply {
                    text = "$scored"
                    textSize = 16f
                    setTextColor(Color.parseColor("#F39C12"))
                    typeface = android.graphics.Typeface.DEFAULT_BOLD
                }
                row.addView(tvVal)
            }
            isScored && scored == null -> {
                // Crossed out
                tvName.alpha = 0.4f
                tvName.paintFlags = tvName.paintFlags or android.graphics.Paint.STRIKE_THRU_TEXT_FLAG
                tvHint.alpha = 0.4f
                val tvVal = TextView(this).apply {
                    text = "✕"
                    textSize = 14f
                    setTextColor(Color.parseColor("#66FF6666"))
                }
                row.addView(tvVal)
            }
            gs.hasRolled -> {
                // Available - show preview
                val preview = cat.calculate(GameState.dice.toList())
                row.setBackgroundColor(Color.parseColor("#1A27AE60"))

                val tvPreview = TextView(this).apply {
                    text = if (preview > 0) "+$preview" else "0"
                    textSize = 15f
                    setTextColor(
                        if (preview > 0) Color.parseColor("#2ECC71")
                        else Color.parseColor("#55FFFFFF")
                    )
                    typeface = android.graphics.Typeface.DEFAULT_BOLD
                    setPadding(dpToPx(8), 0, 0, 0)
                }

                val btnCross = MaterialButton(this).apply {
                    text = "✕"
                    textSize = 10f
                    setTextColor(Color.parseColor("#E74C3C"))
                    setBackgroundColor(Color.parseColor("#22E74C3C"))
                    cornerRadius = dpToPx(6)
                    setPadding(dpToPx(8), dpToPx(4), dpToPx(8), dpToPx(4))
                    val lp = LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.WRAP_CONTENT,
                        LinearLayout.LayoutParams.WRAP_CONTENT
                    ).apply { setMargins(dpToPx(8), 0, 0, 0) }
                    layoutParams = lp
                    setOnClickListener { confirmCross(cat.id, cat.name) }
                }

                row.addView(tvPreview)
                row.addView(btnCross)
                row.setOnClickListener { scoreCategory(cat.id) }
            }
            else -> {
                val tvVal = TextView(this).apply {
                    text = "—"
                    textSize = 14f
                    setTextColor(Color.parseColor("#33FFFFFF"))
                }
                row.addView(tvVal)
            }
        }

        // Divider
        val wrapper = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        wrapper.addView(row)
        val div = View(this).apply {
            setBackgroundColor(Color.parseColor("#0FFFFFFF"))
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dpToPx(1)
            )
        }
        wrapper.addView(div)
        container.addView(wrapper)
    }

    // ===== SCORING =====

    private fun scoreCategory(catId: String) {
        if (!GameState.hasRolled) return
        val player = GameState.currentPlayerObj()
        if (player.scores.containsKey(catId)) return

        val cat = Categories.all.first { it.id == catId }
        player.scores[catId] = cat.calculate(GameState.dice.toList())
        advanceTurn()
    }

    private fun confirmCross(catId: String, catName: String) {
        AlertDialog.Builder(this, R.style.KniffelDialog)
            .setTitle("$catName streichen?")
            .setMessage("Diese Kategorie wird mit 0 Punkten gestrichen.")
            .setPositiveButton("Streichen") { _, _ ->
                val player = GameState.currentPlayerObj()
                if (!player.scores.containsKey(catId)) {
                    player.scores[catId] = null
                    advanceTurn()
                }
            }
            .setNegativeButton("Abbrechen", null)
            .show()
    }

    private fun advanceTurn() {
        if (GameState.isGameOver()) {
            startActivity(Intent(this, EndActivity::class.java))
            finish()
            return
        }
        GameState.nextTurn()
        renderAll()
        // Scroll to top of score sheet
        findViewById<ScrollView>(R.id.scrollView).smoothScrollTo(0, 0)
    }

    private fun dpToPx(dp: Int): Int = (dp * resources.displayMetrics.density).toInt()
}
