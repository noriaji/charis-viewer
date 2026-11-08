// Standalone test of the translation helpers that need no viewer code (research/228). Build and run with run_test.bat.
#include "langdetect.h"
#include "textsplit.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace plus::translate;

static int sFailed = 0;
#define CHECK(c)                                                              \
    do                                                                        \
    {                                                                         \
        if (!(c))                                                             \
        {                                                                     \
            printf("FAIL line %d: %s\n", __LINE__, #c);                       \
            ++sFailed;                                                        \
        }                                                                     \
    } while (0)

static void language_detection()
{
    // English: sentences, chat words, contractions
    CHECK(detectLanguage("Hello, how are you today?") == "en");
    CHECK(detectLanguage("I'm just waiting for my friend to log in.") == "en");
    CHECK(detectLanguage("Thanks for the invite, I'll be right there.") == "en");
    CHECK(detectLanguage("Where did you get that hair?") == "en");
    CHECK(detectLanguage("wanna go to the club?") == "en");
    CHECK(detectLanguage("Let's meet at the sim") == "en");
    CHECK(detectLanguage("lol") == "en");
    CHECK(detectLanguage("ok") == "en");
    CHECK(detectLanguage("thanks!") == "en");
    CHECK(detectLanguage("I love this dress \xE3\x81\x82\xE3\x82\x8A\xE3\x81\x8C") == "en");   // a few kana among English words
    CHECK(detectLanguage("I\xE2\x80\x99m here") == "en");                                        // a typographic apostrophe

    // other languages in Latin letters: not sure it is English, so "" (not translated)
    CHECK(detectLanguage("Hola, \xC2\xBF" "c\xC3\xB3mo est\xC3\xA1s hoy?").empty());
    CHECK(detectLanguage("Bonjour, comment allez-vous ?").empty());
    CHECK(detectLanguage("Ich habe keine Zeit").empty());
    CHECK(detectLanguage("Das ist gut").empty());
    CHECK(detectLanguage("Muchas gracias por todo").empty());
    CHECK(detectLanguage("Obrigado pela ajuda").empty());
    CHECK(detectLanguage("No s\xC3\xA9 qu\xC3\xA9 hacer").empty());   // "no" is English too, the accents decide
    CHECK(detectLanguage("ok gracias").empty());                       // a mix: not sure
    // Spanish, French and Italian without accents: words English shares with them ("a", "no", "me", "on") do not count
    CHECK(detectLanguage("no me gusta la comida a mi").empty());
    CHECK(detectLanguage("me gusta").empty());
    CHECK(detectLanguage("a ver").empty());
    CHECK(detectLanguage("no hay").empty());
    CHECK(detectLanguage("on y va").empty());
    CHECK(detectLanguage("no").empty());
    CHECK(detectLanguage("I have a plan") == "en");
    // short English with words that other languages share: they count when nothing points to another language
    CHECK(detectLanguage("no problem") == "en");
    CHECK(detectLanguage("go to the store") == "en");
    CHECK(detectLanguage("it is what it is") == "en");
    CHECK(detectLanguage("he is so cute") == "en");
    CHECK(detectLanguage("let me go to sleep") == "en");
    CHECK(detectLanguage("a lot of fun") == "en");
    CHECK(detectLanguage("um ok") == "en");
    CHECK(detectLanguage("I am here") == "en");
    CHECK(detectLanguage("see you") == "en");
    CHECK(detectLanguage("me gusta mucho a mi").empty());   // the same shared words, with Spanish around them
    CHECK(detectLanguage("so what") == "en");
    CHECK(detectLanguage("is it ok") == "en");
    // lines of nearby chat that were not translated (2026-11-02): an avatar's question, an object's /me line (the viewer hands
    // over the text after "/me "; with the object's name in front it is English too), short lines of four or five words
    CHECK(detectLanguage("Woowww, How did you get so big? Did they give you some Vitamin boosters??") == "en");
    CHECK(detectLanguage("wants your attention... Now!") == "en");
    CHECK(detectLanguage("Pum wants your attention... Now!") == "en");
    CHECK(detectLanguage("KittyCats - Pum wants your attention... Now!") == "en");
    CHECK(detectLanguage("Pum wants attention now") == "en");
    CHECK(detectLanguage("did you see that") == "en");
    CHECK(detectLanguage("nothing to see here") == "en");
    // the words added then do not make other languages English
    CHECK(detectLanguage("am Montag").empty());                     // German: "am" is not counted as English
    CHECK(detectLanguage("ich bin noch still").empty());
    CHECK(detectLanguage("Woowww").empty());                        // one word that is no English word
    // names, strings of letters, nothing to translate
    CHECK(detectLanguage("Kimberly Resident").empty());
    CHECK(detectLanguage("k").empty());
    CHECK(detectLanguage("123 456").empty());
    CHECK(detectLanguage("").empty());
    CHECK(detectLanguage("...!!!").empty());
    CHECK(detectLanguage("\xF0\x9F\x91\x8D").empty());   // an emoji

    // by script
    CHECK(detectLanguage("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\xE3\x80\x81\xE4\xBB\x8A\xE6\x97\xA5\xE3\x81\xAF\xE5\x85\x83\xE6\xB0\x97\xE3\x81\xA7\xE3\x81\x99\xE3\x81\x8B\xEF\xBC\x9F") == "ja");
    CHECK(detectLanguage("\xE3\x82\x82\xE3\x82\x89\xE3\x81\x84\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F") == "ja");
    CHECK(detectLanguage("\xE8\x8D\x89") == "zh");   // one kanji: Chinese or Japanese, not told apart, not translated
    CHECK(detectLanguage("\xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94") == "ko");
    CHECK(detectLanguage("\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xBB\x8A\xE5\xA4\xA9\xE6\x80\x8E\xE4\xB9\x88\xE6\xA0\xB7") == "zh");
    CHECK(detectLanguage("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \xD0\xBA\xD0\xB0\xD0\xBA \xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB0?") == "ru");
    // a bad byte does not stop it
    CHECK(detectLanguage(std::string("Hello you ") + char(0xFF) + " there") == "en");
}

static std::string join(const std::vector<TextPiece>& pieces)
{
    std::string s;
    for (const TextPiece& p : pieces)
    {
        s += p.text;
    }
    return s;
}

// the pieces to translate, one string: "<a>|<b>"
static std::string translated(const std::vector<TextPiece>& pieces)
{
    std::string s;
    for (const TextPiece& p : pieces)
    {
        if (p.translate)
        {
            s += (s.empty() ? "" : "|") + p.text;
        }
    }
    return s;
}

static void text_splitting()
{
    const char* const lines[] = {
        "Hello there",
        "Check http://example.com/a?b=1, it's great",
        "visit www.example.com.",
        "secondlife:///app/agent/00000000-0000-0000-0000-000000000000/about is here",
        "/me waves hello",
        "(see https://example.com/x) thanks",
        "  leading and trailing  ",
        "https://example.com",
        "",
        "...",
        "\xE8\xA6\x8B\xE3\x81\xA6 https://example.com/\xE8\xB7\xAF \xE3\x81\xA7\xE3\x81\x99",
    };
    // putting the pieces together gives the line back
    for (const char* line : lines)
    {
        CHECK(join(splitForTranslation(line)) == line);
    }

    CHECK(translated(splitForTranslation("Hello there")) == "Hello there");
    CHECK(translated(splitForTranslation("Check http://example.com/a?b=1, it's great")) == "Check|, it's great");
    CHECK(translated(splitForTranslation("visit www.example.com.")) == "visit");
    CHECK(translated(splitForTranslation("secondlife:///app/agent/00000000-0000-0000-0000-000000000000/about is here")) == "is here");
    CHECK(translated(splitForTranslation("/me waves hello")) == "waves hello");
    CHECK(translated(splitForTranslation("/menu is open")) == "/menu is open");   // not an action
    CHECK(translated(splitForTranslation("(see https://example.com/x) thanks")) == "(see|) thanks");
    CHECK(translated(splitForTranslation("  leading and trailing  ")) == "leading and trailing");
    CHECK(translated(splitForTranslation("https://example.com")).empty());
    CHECK(translated(splitForTranslation("")).empty());
    CHECK(translated(splitForTranslation("...")).empty());                         // no letters: nothing to translate
    // an address ends where non-ASCII text starts; the text after it is translated
    CHECK(translated(splitForTranslation("\xE8\xA6\x8B\xE3\x81\xA6 https://example.com/\xE8\xB7\xAF \xE3\x81\xA7\xE3\x81\x99")) ==
          "\xE8\xA6\x8B\xE3\x81\xA6|\xE8\xB7\xAF \xE3\x81\xA7\xE3\x81\x99");
    // an upper case scheme is an address too, and one in the middle of a word is not
    CHECK(translated(splitForTranslation("see HTTP://EXAMPLE.COM now")) == "see|now");
    CHECK(translated(splitForTranslation("nothttp://x")) == "nothttp://x");
    // "www." with nothing after it is no address and keeps its dot; an address may follow Japanese text directly
    CHECK(translated(splitForTranslation("visit www. now")) == "visit www. now");
    CHECK(join(splitForTranslation("visit www. now")) == "visit www. now");
    CHECK(translated(splitForTranslation("\xE8\xA6\x8B\xE3\x81\xA6https://example.com/x \xE3\x81\xAD")) == "\xE8\xA6\x8B\xE3\x81\xA6|\xE3\x81\xAD");
    CHECK(join(splitForTranslation("\xE8\xA6\x8B\xE3\x81\xA6https://example.com/x \xE3\x81\xAD")) == "\xE8\xA6\x8B\xE3\x81\xA6https://example.com/x \xE3\x81\xAD");
    // the pieces to translate have no white space at the ends
    for (const TextPiece& p : splitForTranslation("  a  http://x.y  b  "))
    {
        if (p.translate)
        {
            CHECK(p.text == "a" || p.text == "b");
        }
    }
}

int main()
{
    language_detection();
    text_splitting();
    printf(sFailed ? "FAILED (%d)\n" : "all passed\n", sFailed);
    return sFailed ? 1 : 0;
}
