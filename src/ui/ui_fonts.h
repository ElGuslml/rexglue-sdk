/**
 * @file        ui/ui_fonts.h
 * @brief       The dialogs' fonts, embedded from resources/fonts (internal)
 *
 * @license     BSD 3-Clause License, as the rest of the SDK (LICENSE in the project root). Added on the
 *              recomp branch of ElGuslml/rexglue-sdk (RECOMP.md). The fonts themselves are Barlow, under the
 *              SIL Open Font License 1.1 (resources/fonts/OFL.txt).
 */
#pragma once

#include <cstddef>

namespace rex::ui::fonts {

// TrueType files, defined in the ui_fonts_data.cpp that src/ui/CMakeLists.txt generates.
extern const unsigned char kBarlowRegular[];
extern const size_t kBarlowRegularSize;
extern const unsigned char kBarlowSemiBold[];
extern const size_t kBarlowSemiBoldSize;
extern const unsigned char kBarlowCondensedBold[];
extern const size_t kBarlowCondensedBoldSize;

}  // namespace rex::ui::fonts
