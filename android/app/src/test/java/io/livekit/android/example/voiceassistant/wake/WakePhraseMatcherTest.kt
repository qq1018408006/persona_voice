package io.livekit.android.example.voiceassistant.wake

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class WakePhraseMatcherTest {

    @Test
    fun recognizesWakePhraseWithPunctuationAndContext() {
        assertTrue(WakePhraseMatcher.containsWakePhrase("贾维斯！帮我查一下天气"))
        assertTrue(WakePhraseMatcher.containsWakePhrase("嗨，贾 维 斯。"))
        assertTrue(WakePhraseMatcher.containsWakePhrase("假维斯"))
        assertTrue(WakePhraseMatcher.containsWakePhrase("嘉维斯"))
        assertTrue(WakePhraseMatcher.containsWakePhrase("贾威斯"))
        assertTrue(WakePhraseMatcher.containsWakePhrase("加V斯"))
        assertTrue(WakePhraseMatcher.containsWakePhrase("贾维士"))
        assertTrue(WakePhraseMatcher.containsWakePhrase("杰维斯"))
        assertTrue(WakePhraseMatcher.containsWakePhrase("杰威斯"))
        assertTrue(WakePhraseMatcher.containsWakePhrase("捷维斯"))
        assertTrue(WakePhraseMatcher.containsWakePhrase("Hey Jarvis"))
        assertFalse(WakePhraseMatcher.containsWakePhrase("休息吧"))
        assertFalse(WakePhraseMatcher.containsWakePhrase("加微信"))
    }

    @Test
    fun recognizesSleepPhraseWithPunctuationAndContext() {
        assertTrue(WakePhraseMatcher.containsSleepPhrase("好了，休息吧。"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("休 息 巴！"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("休息罢"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("你休息一下吧"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("休息了吧"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("休息会吧"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("休息一会儿吧"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("休息会儿吧"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("歇会吧"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("歇会儿吧"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("我要休息了"))
        assertTrue(WakePhraseMatcher.containsSleepPhrase("准备休息吧"))
        assertFalse(WakePhraseMatcher.containsSleepPhrase("闭嘴"))
        assertFalse(WakePhraseMatcher.containsSleepPhrase("今天可以休息"))
        assertFalse(WakePhraseMatcher.containsSleepPhrase("休息时间"))
        assertFalse(WakePhraseMatcher.containsSleepPhrase("休息会"))
        assertFalse(WakePhraseMatcher.containsSleepPhrase("贾维斯"))
    }

    @Test
    fun exposesWakePhraseVariantsForRecognizerBiasing() {
        assertTrue(WakePhraseMatcher.recognitionBiases.contains("贾维斯"))
        assertTrue(WakePhraseMatcher.recognitionBiases.contains("贾威斯"))
        assertTrue(WakePhraseMatcher.recognitionBiases.contains("加V斯"))
        assertTrue(WakePhraseMatcher.recognitionBiases.contains("我要休息了"))
    }
}
