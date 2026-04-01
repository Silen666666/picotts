package com.example.kniffel.model

data class Category(
    val id: String,
    val name: String,
    val hint: String,
    val section: String,
    val calculate: (List<Int>) -> Int
)

object Categories {
    private fun sumOf(dice: List<Int>, value: Int) = dice.filter { it == value }.sum()
    private fun total(dice: List<Int>) = dice.sum()
    private fun counts(dice: List<Int>) = dice.groupingBy { it }.eachCount()
    private fun hasN(dice: List<Int>, n: Int) = counts(dice).values.any { it >= n }

    private fun isFullHouse(dice: List<Int>): Boolean {
        val c = counts(dice).values.sorted()
        return c.size == 2 && c[0] == 2 && c[1] == 3
    }

    private fun isSmallStraight(dice: List<Int>): Boolean {
        val unique = dice.toSortedSet()
        return listOf(
            setOf(1, 2, 3, 4),
            setOf(2, 3, 4, 5),
            setOf(3, 4, 5, 6)
        ).any { unique.containsAll(it) }
    }

    private fun isLargeStraight(dice: List<Int>): Boolean {
        val unique = dice.toSortedSet().toList()
        return unique.size == 5 && unique.last() - unique.first() == 4
    }

    val all = listOf(
        Category("ones",            "Einser",       "Summe aller Einsen",          "upper") { sumOf(it, 1) },
        Category("twos",            "Zweier",       "Summe aller Zweien",          "upper") { sumOf(it, 2) },
        Category("threes",          "Dreier",       "Summe aller Dreien",          "upper") { sumOf(it, 3) },
        Category("fours",           "Vierer",       "Summe aller Vieren",          "upper") { sumOf(it, 4) },
        Category("fives",           "Fünfer",       "Summe aller Fünfen",          "upper") { sumOf(it, 5) },
        Category("sixes",           "Sechser",      "Summe aller Sechsen",         "upper") { sumOf(it, 6) },
        Category("three_of_a_kind", "Dreierpasch",  "3 gleiche → Gesamtsumme",     "lower") { if (hasN(it, 3)) total(it) else 0 },
        Category("four_of_a_kind",  "Viererpasch",  "4 gleiche → Gesamtsumme",     "lower") { if (hasN(it, 4)) total(it) else 0 },
        Category("full_house",      "Full House",   "Dreierpasch + Pärchen → 25",  "lower") { if (isFullHouse(it)) 25 else 0 },
        Category("small_straight",  "Kl. Straße",   "4 aufeinander → 30",          "lower") { if (isSmallStraight(it)) 30 else 0 },
        Category("large_straight",  "Gr. Straße",   "5 aufeinander → 40",          "lower") { if (isLargeStraight(it)) 40 else 0 },
        Category("kniffel",         "Kniffel",      "5 gleiche → 50",              "lower") { if (hasN(it, 5)) 50 else 0 },
        Category("chance",          "Chance",       "Summe aller Würfel",          "lower") { total(it) }
    )
}
