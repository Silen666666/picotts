package com.example.kniffel.model

object GameState {

    data class Player(
        val name: String,
        val index: Int,
        val scores: MutableMap<String, Int?> = mutableMapOf()
    )

    var players: List<Player> = emptyList()
    var currentPlayer: Int = 0
    var rollsLeft: Int = 3
    var dice: IntArray = IntArray(5) { 1 }
    var held: BooleanArray = BooleanArray(5)
    var hasRolled: Boolean = false
    var round: Int = 1

    fun reset(playerNames: List<String>) {
        players = playerNames.mapIndexed { i, name -> Player(name, i) }
        currentPlayer = 0
        rollsLeft = 3
        dice = IntArray(5) { 1 }
        held = BooleanArray(5)
        hasRolled = false
        round = 1
    }

    fun currentPlayerObj(): Player = players[currentPlayer]

    fun calcUpperScore(player: Player): Int =
        Categories.all.filter { it.section == "upper" }
            .sumOf { player.scores[it.id] ?: 0 }

    fun calcTotalScore(player: Player): Int {
        val upper = calcUpperScore(player)
        val bonus = if (upper >= 63) 35 else 0
        val lower = Categories.all.filter { it.section == "lower" }
            .sumOf { player.scores[it.id] ?: 0 }
        return upper + bonus + lower
    }

    fun isGameOver(): Boolean =
        players.all { p -> Categories.all.all { c -> p.scores.containsKey(c.id) } }

    fun nextTurn() {
        var next = (currentPlayer + 1) % players.size
        val maxLoops = players.size
        var loops = 0
        while (Categories.all.all { c -> players[next].scores.containsKey(c.id) } && loops < maxLoops) {
            next = (next + 1) % players.size
            loops++
        }
        if (next <= currentPlayer) round++
        currentPlayer = next
        rollsLeft = 3
        dice = IntArray(5) { 1 }
        held = BooleanArray(5)
        hasRolled = false
    }
}
