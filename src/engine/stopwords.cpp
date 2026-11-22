#include "stopwords.h"

#include <sstream>

namespace vfts {

namespace {

struct List { const char *code; const char *words; };

// English: the default stop set of Lucene's EnglishAnalyzer. The others are short lists of the
// most frequent function words of each language.
const List LISTS[] = {
    {"en", "a an and are as at be but by for if in into is it no not of on or such that the their then there "
           "these they this to was will with"},
    {"fr", "au aux avec ce ces dans de des du elle en et eux il je la le les leur lui ma mais me même mes moi mon "
           "ne nos notre nous on ou par pas pour qu que qui sa se ses son sur ta te tes toi ton tu un une vos votre "
           "vous c d j l à m n s t y été étée étées étés"},
    {"de", "aber alle als also am an auch auf aus bei bin bis bist da damit dann das dass dein deine dem den der "
           "des dich die dir doch du durch ein eine einem einen einer eines er es für hat hatte ich ihr im in ist "
           "ja kann mich mir mit nach nicht noch nun nur ob oder sein sich sie sind so über um und uns unter von "
           "vor war was weil wenn wer wie wir wird zu zum zur"},
    {"es", "a al algo como con de del el ella en entre era es esta este fue ha hay la las le les lo los más me mi "
           "muy no nos o para pero por que se si sin sobre su sus también te tu un una uno y ya"},
    {"it", "a ad al alla alle anche che chi ci come con da dal dalla de del della delle di e ed gli ha i il in io "
           "la le lo ma mi ne nel nella non o per più se si sono su sua suo tra tu un una uno"},
    {"pt", "a ao aos as com como da das de do dos e ela ele em entre era essa esse está foi há isso mais mas me "
           "muito na nas no nos não o os ou para pela pelo por que se sem seu sua são também te um uma à às é"},
    {"nl", "aan al als bij dan dat de die dit door een en er had heb heeft het hij hoe hun ik in is je kan maar "
           "met mij niet nog of om ook op over te tot uit van voor was wat we wel wij zal ze zich zij zijn"},
    {"ru", "а без бы был была были было в вам вас весь во вот все всё вы да для до его ее её если есть еще ещё же "
           "за и из или им их к как ко когда кто ли мне мы на над не нет ни но о об от по под при с со так там то "
           "только у уже чем что чтобы эта это я"},
    {"ar", "في من على إلى عن مع هذا هذه ذلك التي الذي التى الذى أن إن كان كانت لم لا ما هو هي و أو ثم قد كل بعض بين "
           "عند حتى إذا"},
    {"fa", "و در به از که این آن با را برای است بود شد می تا هم یا اما اگر هر"},
    {"he", "של את על עם זה זו הוא היא הם הן אני אתה גם לא כי אם או כל אבל רק עוד יש אין היה הייתה מה מי"},
};

} // namespace

const char *stopword_list(std::string_view code)
{
    for (const List &l : LISTS) if (code == l.code) return l.words;
    return "";
}

Stopwords::Stopwords(Lang lang, Analyzer &analyzer)
{
    std::istringstream in(stopword_list(lang_info(lang).code));
    std::string w, norm;
    while (in >> w) {
        analyzer.normalize_piece(w, lang, norm);
        if (!norm.empty()) set_.insert(norm);
    }
}

} // namespace vfts
