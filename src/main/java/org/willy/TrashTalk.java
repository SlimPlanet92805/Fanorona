package org.willy;

import java.util.Random;

/**
 * The AI's running commentary, picked from the search result rather than
 * computed by it.
 * <p>
 * This lives outside {@link AIPlayer} on purpose: it is presentation, not
 * search, and the planned C++/WebAssembly port only carries the engine across.
 * Keeping it here means the port has ~160 fewer lines of string tables to
 * transliterate, and the server stays free to render a result however it likes.
 */
class TrashTalk {

    private static final Random RND = new Random();

    /**
     * @param score        evaluation of the move just chosen, from the AI's side
     * @param prevScore    evaluation after the AI's previous move, so a sudden
     *                     jump can be read as the human having blundered
     * @param isMate       the score is past the mate threshold
     * @param feedback     prediction hit/miss marker from {@code analyzeHumanMove}
     * @param lang         {@code "en"} for English, anything else for Chinese
     */
    static String pick(int score, int prevScore, boolean isMate, String feedback, String lang) {
        int diff = score - prevScore;
        boolean amWinning = score > 300;
        boolean en = "en".equalsIgnoreCase(lang);

        // --- Scenario A: Checkmate ---
        if (isMate) {
            if (score > 0) {
                return choose(en ? new String[]{
                        "GG, that's a wrap. 😂",
                        "Easiest win of my life. 🥳",
                        "Absolutely demolished. ⚔️",
                        "You just donated your whole army. 💀",
                        "Is that really your best? 😏",
                        "Not bad. Never do it again. 🤡",
                        "That's it? That's really it? 🤷",
                        "Please tell me that was a joke. 🤡",
                        "Go practice and come back. ♿",
                        "It's over. Better luck next time. ☕"
                } : new String[]{
                        "蚌埠住了，这也能输？😂",
                        "赢麻了家人们 🥳",
                        "杀杀杀杀杀！ ⚔️",
                        "纯纯的送 💀",
                        "纯纯的乱杀 🎯",
                        "这就是你的全部实力吗？😏",
                        "下得不错，下次别下了。🤡",
                        "就这？就这？ 🤷",
                        "你是来搞笑的吧？ 🤡",
                        "建议多练练再来 ♿",
                        "结束了，下把加油吧。☕"
                });
            }
            // AI losing (rare)
            return choose(en ? new String[]{
                    "Wait, WHAT? I'm losing to this?? Reported. 🚨",
                    "Hacks. Has to be hacks. Reported. 🚨"
            } : new String[]{
                    "不是哥们，开了？这都能输？举报了！🚨",
                    "卧槽？外挂！举报了 🚨"
            });
        }

        // --- Scenario B: Player Blunder (AI score spiked up by > 400) ---
        if (diff > 400 && amWinning) {
            return choose(en ? new String[]{
                    "Thanks, I'll remember this move forever. 🙏",
                    "Are we playing chess or are you just gifting pieces? 🎁",
                    "I was still planning my win and you just handed it to me. 🤝",
                    "That move sent me. 😂",
                    "Is that seriously your level? 🐔",
                    "Textbook donation right there. 📦",
                    "Stop giving pieces away, I can't eat any more. 🍽️",
                    "My win rate just skyrocketed off that. 📈",
                    "Was that a misclick or did you just give up? 🤨"
            } : new String[]{
                    "谢谢你，这步我记一辈子。🙏",
                    "兄弟你这是在下棋，还是在送？🎁",
                    "我刚还在想怎么赢，你就帮我想好了。🤝",
                    "你这一手我直接笑死 😂",
                    "这就是你的实力？ 🐔",
                    "典中典之送子大师 📦",
                    "别送了别送了，我吃不下了 🍽️",
                    "这步一出，胜率直接起飞。📈",
                    "你刚刚那下，是手滑还是心软？🤨"
            });
        }

        // --- Scenario C: Prediction Hit (AI read the player) ---
        if (feedback != null && feedback.contains("Hit") && amWinning) {
            return choose(en ? new String[]{
                    "Yeah, I already knew you'd play that. 📖",
                    "Didn't even need to calculate, that was obvious. 😑",
                    "Same old pattern, as expected. 🥱",
                    "I can see right through you. 👁️",
                    "Called it. 🎯",
                    "Way too predictable, no challenge at all. 🥱",
                    "I learned this line in grade school. 📘",
                    "Relax, I already know what you're planning next. 🔮",
                    "Are you following my script or something? 🎬"
            } : new String[]{
                    "想什么呢，我就知道你会走这步。📖",
                    "我都不用算，你肯定这么下。😑",
                    "果然，老套路了。🥱",
                    "你的想法被我看穿了 👁️",
                    "早就猜到你要这么走 🎯",
                    "太好猜了，没挑战性 🥱",
                    "你这思路，我小学就会了。📘",
                    "别急，我知道你下一步想干嘛。🔮",
                    "你是不是在按我剧本走？🎬"
            });
        }

        // --- Scenario D: AI is Crushing (> 2000 score) ---
        if (score > 2000) {
            return choose(en ? new String[]{
                    "This isn't even a skill issue anymore. 😶",
                    "Honestly, just start a new game. 🏳️",
                    "Surrender already, stop suffering. 🏳️",
                    "I'm basically running tutorial mode over here. 📚",
                    "Every move just makes it worse. 😬"
            } : new String[]{
                    "这盘已经不是技术问题了。😶",
                    "建议直接下一把，真的。🏳️",
                    "投了吧，别挣扎了 🏳️",
                    "我这边显示的是教学模式。📚",
                    "别下了，越下越难看。😬"
            });
        }

        // --- Scenario E: AI Disadvantage (Rare) ---
        if (score < -500) {
            return choose(en ? new String[]{
                    "Don't celebrate yet, I'm not even trying. 😏",
                    "Giving you a head start just to see what you've got. 🧐",
                    "Consider this a handicap match. 📉",
                    "I'm just letting you have a few moves. 🤨"
            } : new String[]{
                    "别高兴太早，我还没认真呢。😏",
                    "让你几步，测试一下你成色。🧐",
                    "现在是让分局，懂？📉",
                    "只是让你几步罢了 🤨"
            });
        }

        // --- Scenario F: Neutral / Thinking ---
        return choose(en ? new String[]{
                "Let me calculate... hold on. 🤔",
                "Let me take a look... 🧐",
                "Hmm, this is getting interesting. 😐",
                "Figuring out how to make this loss look dignified for you... 💭",
                "Computing your 108 possible ways to lose... 🌌",
                "Hang on, I'll get you sorted properly. 🤖",
                "Guess where I'm moving next. 🎲",
                "Patience, almost there. ⏰",
                "Hmm... what are you even trying to do? 😕",
                "Go on, show me what you've got. 👀"
        } : new String[]{
                "我算算……你先别急。🤔",
                "让我康康... 🧐",
                "嗯……这盘有点意思。😐",
                "让我想想怎么让你输得体面点... 💭",
                "正在计算你的108种死法... 🌌",
                "别急，等我给你安排得明明白白。🤖",
                "你猜我下一步走哪？ 🎲",
                "别催别催，马上 ⏰",
                "嗯... 你这是要干嘛？ 😕",
                "继续，下给我看看。👀"
        });
    }

    private static String choose(String[] msgs) {
        return msgs[RND.nextInt(msgs.length)];
    }
}
