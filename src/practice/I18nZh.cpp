// Chinese translations of practice-module strings.
#include "common/I18n.h"
#include "practice/Practice.h"

namespace gs {

void registerPracticeTranslations() {
    static const TrPair kZh[] = {
        // judgeName()
        {"Perfect", "完美"},
        {"Good", "很好"},
        {"OK", "还行"},
        {"Wrong note", "音不对"},
        {"Missed", "漏掉了"},
    };
    addTranslations(kZh);
}

}  // namespace gs
