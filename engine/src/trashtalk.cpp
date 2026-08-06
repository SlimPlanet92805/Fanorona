// The AI's commentary. Ported from TrashTalk.java so the C++ build is a
// complete replacement for the Java server rather than a headless engine.
#include "trashtalk.h"

#include <random>

namespace fanorona {

namespace {

std::mt19937& rng() {
    static thread_local std::mt19937 gen{std::random_device{}()};
    return gen;
}

const char* choose(const std::vector<const char*>& msgs) {
    std::uniform_int_distribution<size_t> d(0, msgs.size() - 1);
    return msgs[d(rng())];
}

}  // namespace

const char* trash_talk(int score, int prevScore, bool isMate, const std::string& feedback,
                       bool english) {
    int diff = score - prevScore;
    bool amWinning = score > 300;

    // --- Checkmate ---
    if (isMate) {
        if (score > 0)
            return choose(english ? std::vector<const char*>{
                "GG, that's a wrap. \xF0\x9F\x98\x82",
                "Easiest win of my life. \xF0\x9F\xA5\xB3",
                "Absolutely demolished. \xE2\x9A\x94\xEF\xB8\x8F",
                "You just donated your whole army. \xF0\x9F\x92\x80",
                "Is that really your best? \xF0\x9F\x98\x8F",
                "Not bad. Never do it again. \xF0\x9F\xA4\xA1",
                "That's it? That's really it? \xF0\x9F\xA4\xB7",
                "Go practice and come back. \xE2\x99\xBF",
                "It's over. Better luck next time. \xE2\x98\x95"}
                : std::vector<const char*>{
                "蚌埠住了，这也能输？\xF0\x9F\x98\x82",
                "赢麻了家人们 \xF0\x9F\xA5\xB3",
                "杀杀杀杀杀！ \xE2\x9A\x94\xEF\xB8\x8F",
                "纯纯的送 \xF0\x9F\x92\x80",
                "这就是你的全部实力吗？\xF0\x9F\x98\x8F",
                "下得不错，下次别下了。\xF0\x9F\xA4\xA1",
                "就这？就这？ \xF0\x9F\xA4\xB7",
                "建议多练练再来 \xE2\x99\xBF",
                "结束了，下把加油吧。\xE2\x98\x95"});
        return choose(english ? std::vector<const char*>{
            "Wait, WHAT? I'm losing to this?? Reported. \xF0\x9F\x9A\xA8",
            "Hacks. Has to be hacks. Reported. \xF0\x9F\x9A\xA8"}
            : std::vector<const char*>{
            "不是哥们，开了？这都能输？举报了！\xF0\x9F\x9A\xA8",
            "卧槽？外挂！举报了 \xF0\x9F\x9A\xA8"});
    }

    // --- Player blunder: the score jumped in our favour ---
    if (diff > 400 && amWinning)
        return choose(english ? std::vector<const char*>{
            "Thanks, I'll remember this move forever. \xF0\x9F\x99\x8F",
            "Are we playing Fanorona or are you just gifting pieces? \xF0\x9F\x8E\x81",
            "I was still planning my win and you handed it to me. \xF0\x9F\xA4\x9D",
            "That move sent me. \xF0\x9F\x98\x82",
            "Textbook donation right there. \xF0\x9F\x93\xA6",
            "Stop giving pieces away, I can't eat any more. \xF0\x9F\x8D\xBD",
            "My win rate just skyrocketed off that. \xF0\x9F\x93\x88",
            "Was that a misclick or did you just give up? \xF0\x9F\xA4\xA8"}
            : std::vector<const char*>{
            "谢谢你，这步我记一辈子。\xF0\x9F\x99\x8F",
            "兄弟你这是在下棋，还是在送？\xF0\x9F\x8E\x81",
            "我刚还在想怎么赢，你就帮我想好了。\xF0\x9F\xA4\x9D",
            "你这一手我直接笑死 \xF0\x9F\x98\x82",
            "典中典之送子大师 \xF0\x9F\x93\xA6",
            "别送了别送了，我吃不下了 \xF0\x9F\x8D\xBD",
            "这步一出，胜率直接起飞。\xF0\x9F\x93\x88",
            "你刚刚那下，是手滑还是心软？\xF0\x9F\xA4\xA8"});

    // --- We predicted the human's move ---
    if (feedback.find("Hit") != std::string::npos && amWinning)
        return choose(english ? std::vector<const char*>{
            "Yeah, I already knew you'd play that. \xF0\x9F\x93\x96",
            "Didn't even need to calculate, that was obvious. \xF0\x9F\x98\x91",
            "Same old pattern, as expected. \xF0\x9F\xA5\xB1",
            "I can see right through you. \xF0\x9F\x91\x81",
            "Called it. \xF0\x9F\x8E\xAF",
            "Relax, I already know what you're planning next. \xF0\x9F\x94\xAE",
            "Are you following my script or something? \xF0\x9F\x8E\xAC"}
            : std::vector<const char*>{
            "想什么呢，我就知道你会走这步。\xF0\x9F\x93\x96",
            "我都不用算，你肯定这么下。\xF0\x9F\x98\x91",
            "果然，老套路了。\xF0\x9F\xA5\xB1",
            "你的想法被我看穿了 \xF0\x9F\x91\x81",
            "早就猜到你要这么走 \xF0\x9F\x8E\xAF",
            "别急，我知道你下一步想干嘛。\xF0\x9F\x94\xAE",
            "你是不是在按我剧本走？\xF0\x9F\x8E\xAC"});

    // --- Crushing ---
    if (score > 2000)
        return choose(english ? std::vector<const char*>{
            "This isn't even a skill issue anymore. \xF0\x9F\x98\xB6",
            "Honestly, just start a new game. \xF0\x9F\x8F\xB3\xEF\xB8\x8F",
            "Surrender already, stop suffering. \xF0\x9F\x8F\xB3\xEF\xB8\x8F",
            "I'm basically running tutorial mode over here. \xF0\x9F\x93\x9A",
            "Every move just makes it worse. \xF0\x9F\x98\xAC"}
            : std::vector<const char*>{
            "这盘已经不是技术问题了。\xF0\x9F\x98\xB6",
            "建议直接下一把，真的。\xF0\x9F\x8F\xB3\xEF\xB8\x8F",
            "投了吧，别挣扎了 \xF0\x9F\x8F\xB3\xEF\xB8\x8F",
            "我这边显示的是教学模式。\xF0\x9F\x93\x9A",
            "别下了，越下越难看。\xF0\x9F\x98\xAC"});

    // --- Losing (rare) ---
    if (score < -500)
        return choose(english ? std::vector<const char*>{
            "Don't celebrate yet, I'm not even trying. \xF0\x9F\x98\x8F",
            "Giving you a head start just to see what you've got. \xF0\x9F\xA7\x90",
            "Consider this a handicap match. \xF0\x9F\x93\x89",
            "I'm just letting you have a few moves. \xF0\x9F\xA4\xA8"}
            : std::vector<const char*>{
            "别高兴太早，我还没认真呢。\xF0\x9F\x98\x8F",
            "让你几步，测试一下你成色。\xF0\x9F\xA7\x90",
            "现在是让分局，懂？\xF0\x9F\x93\x89",
            "只是让你几步罢了 \xF0\x9F\xA4\xA8"});

    // --- Neutral ---
    return choose(english ? std::vector<const char*>{
        "Let me calculate... hold on. \xF0\x9F\xA4\x94",
        "Let me take a look... \xF0\x9F\xA7\x90",
        "Hmm, this is getting interesting. \xF0\x9F\x98\x90",
        "Figuring out how to make this loss look dignified for you... \xF0\x9F\x92\xAD",
        "Computing your 108 possible ways to lose... \xF0\x9F\x8C\x8C",
        "Guess where I'm moving next. \xF0\x9F\x8E\xB2",
        "Patience, almost there. \xE2\x8F\xB0",
        "Go on, show me what you've got. \xF0\x9F\x91\x80"}
        : std::vector<const char*>{
        "我算算……你先别急。\xF0\x9F\xA4\x94",
        "让我康康... \xF0\x9F\xA7\x90",
        "嗯……这盘有点意思。\xF0\x9F\x98\x90",
        "让我想想怎么让你输得体面点... \xF0\x9F\x92\xAD",
        "正在计算你的108种死法... \xF0\x9F\x8C\x8C",
        "你猜我下一步走哪？ \xF0\x9F\x8E\xB2",
        "别催别催，马上 \xE2\x8F\xB0",
        "继续，下给我看看。\xF0\x9F\x91\x80"});
}

const char* strategy_name(int score) {
    if (score > MATE_THRESHOLD) return "Checkmate";
    if (score < -MATE_THRESHOLD) return "Defeat";
    if (score >= 2000) return "Crushing";
    if (score >= 500) return "Advantage";
    if (score <= -2000) return "Critical";
    if (score <= -500) return "Pressure";
    return "Balanced";
}

}  // namespace fanorona
