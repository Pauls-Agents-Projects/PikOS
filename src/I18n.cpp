#include "I18n.h"

namespace pixelos {

const char* tr(TextKey key, const String& language) {
  const bool estonian = language == "et";
  switch (key) {
    case TextKey::Inside:
      return estonian ? "Toas" : "Inside";
    case TextKey::Outside:
      return estonian ? "Õues" : "Outside";
    case TextKey::News:
      return estonian ? "Uudised" : "News";
    case TextKey::Battery:
      return estonian ? "Aku" : "Battery";
    case TextKey::NoData:
      return estonian ? "Andmed puuduvad" : "No data";
    case TextKey::Setup:
      return estonian ? "Seadista" : "Setup";
    case TextKey::Address:
      return estonian ? "Aadress" : "Address";
    case TextKey::Sleeping:
      return estonian ? "Unerežiim" : "Sleep";
    case TextKey::Saved:
      return estonian ? "Salvestatud" : "Saved";
    case TextKey::Wifi:
      return "WI-FI";
    case TextKey::Remote:
      return estonian ? "Kaugekraan" : "Remote";
  }
  return "";
}

}  // namespace pixelos
