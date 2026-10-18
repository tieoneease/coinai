package dev.coinai.clips

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SceneSummaryTest {
    private fun card(json: String) = """{"say":"Said.","mood":"happy","card":$json}"""

    @Test
    fun everyKindSummarisesItsSlots() {
        val cases = mapOf(
            """{"kind":"answer","title":"Paris","body":"Capital of France since 987."}""" to "Paris\nCapital of France since 987.",
            """{"kind":"answer","title":"Yes"}""" to "Yes",
            """{"kind":"stat","value":"9","label":"20% of 45"}""" to "9\n20% of 45",
            """{"kind":"stat","value":"72","unit":"kg","label":"Your weight","sub":"down 2 this week"}""" to "72 kg\nYour weight\ndown 2 this week",
            """{"kind":"list","title":"Shopping","items":["Milk","Eggs"]}""" to "Shopping\n- Milk\n- Eggs",
            """{"kind":"steps","title":"Boil an egg","steps":["Boil water","Add egg"]}""" to "Boil an egg\n1. Boil water\n2. Add egg",
            """{"kind":"timer","label":"Tea","seconds":180}""" to "Tea\n3 min",
            """{"kind":"timer","label":"Bake","seconds":5430}""" to "Bake\n1 h 30 min 30 s",
            """{"kind":"later","text":"Remind you at 5"}""" to "Remind you at 5",
        )
        for ((c, expected) in cases) assertEquals(c, expected, SceneSummary.of(card(c)))
    }

    @Test
    fun unusableCardsFallBackToSayOrNothing() {
        assertEquals("Said.", SceneSummary.of(card("""{"kind":"hologram","text":"x"}""")))
        assertEquals("Said.", SceneSummary.of("""{"say":"Said."}"""))
        assertEquals("Said.", SceneSummary.of(card("""{"kind":"answer","title":42}""")))
        assertEquals("", SceneSummary.of("not json"))
        assertEquals("", SceneSummary.of("[]"))
    }

    @Test
    fun laterTextOnlyForLaterCards() {
        assertEquals("Remind you at 5", SceneSummary.laterText(card("""{"kind":"later","text":"Remind you at 5"}""")))
        assertNull(SceneSummary.laterText(card("""{"kind":"answer","title":"Hi"}""")))
        assertNull(SceneSummary.laterText("{"))
    }

    @Test
    fun durations() {
        assertEquals("45 s", SceneSummary.duration(45))
        assertEquals("1 min 30 s", SceneSummary.duration(90))
        assertEquals("1 h", SceneSummary.duration(3600))
        assertEquals("24 h", SceneSummary.duration(86400))
    }
}
