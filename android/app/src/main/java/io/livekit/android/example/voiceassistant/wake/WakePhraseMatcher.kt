package io.livekit.android.example.voiceassistant.wake

object WakePhraseMatcher {
    private val ignoredCharacters = Regex("[\\s，。！？、,.!?~～]")
    private val wakePhrases = listOf(
        "贾维斯",
        "贾维思",
        "贾维丝",
        "贾维士",
        "贾维史",
        "贾威斯",
        "假维斯",
        "假威斯",
        "加维斯",
        "加威斯",
        "家维斯",
        "家威斯",
        "嘉维斯",
        "嘉威斯",
        "杰维斯",
        "杰威斯",
        "捷维斯",
        "贾V斯",
        "假V斯",
        "加V斯",
        "家V斯",
        "嘉V斯",
        "jarvis",
    )
    private val sleepPhrases = listOf(
        "休息吧",
        "休息巴",
        "休息八",
        "休息把",
        "休息罢",
        "休息爸",
        "休息一下吧",
        "休息下吧",
        "休息了吧",
        "休息会吧",
        "休息一会儿吧",
        "休息会儿吧",
        "歇息吧",
        "歇会吧",
        "歇会儿吧",
        "我要休息了",
        "准备休息吧",
    )

    /** Short phrases supplied to recognizers that support contextual biasing. */
    val recognitionBiases: List<String> = (wakePhrases + sleepPhrases).distinct()

    fun normalize(text: String): String =
        text.lowercase().replace(ignoredCharacters, "")

    fun containsWakePhrase(text: String): Boolean =
        normalize(text).let { normalized ->
            wakePhrases.any { normalized.contains(normalize(it)) }
        }

    fun containsSleepPhrase(text: String): Boolean =
        normalize(text).let { normalized ->
            sleepPhrases.any { normalized.contains(normalize(it)) }
        }
}
