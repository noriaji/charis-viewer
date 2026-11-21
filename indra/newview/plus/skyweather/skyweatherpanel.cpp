/**
 * @file skyweatherpanel.cpp
 * @brief The weather tab of the sky editors (research/371-weather-tab.md 3.3, 3.5)
 */

#include "llviewerprecompiledheaders.h"

#include "skyweatherpanel.h"

#include "plus/slwx/slwx.h"
#include "plus/slwx/slwxtag.h"

#include "llcontrol.h"
#include "llsettingssky.h"
#include "llstring.h"
#include "lluictrl.h"
#include "llviewercontrol.h"


// <plus> skyweather-ui
static LLPanelInjector<LLPanelSettingsSkyWeatherTab> t_settings_weather("panel_settings_weather");

namespace
{
    const std::string FIELD_SWITCH("sw_weather");
    const std::string FIELD_NOTICE("l_notice");
    const std::string FIELD_RAIN("s_rain");
    const std::string FIELD_RAIN_HINT("l_rain_hint");
    const std::string FIELD_SNOW("s_snow");
    const std::string FIELD_SNOW_HINT("l_snow_hint");
    const std::string FIELD_WIND("s_wind");
    const std::string FIELD_WIND_DIR("s_wind_dir");
    const std::string FIELD_WIND_VAR("s_wind_var");
    const std::string FIELD_GUST("s_gust");
    const std::string FIELD_WIND_DIR_VAR("s_wind_dir_var");
    const std::string FIELD_IMPORT("b_import");

    const char* const SETTING_OWN_RAIN = "PlusRain";
    const char* const SETTING_OWN_SNOW = "PlusSnow";
    const char* const SETTING_OWN_WIND = "PlusRainWind";
    const char* const SETTING_OWN_WIND_DIR = "PlusRainWindDirection";
    const char* const SETTING_USE_SKY = "PlusSkyWeather";    // plus/skyweather: use the sky's weather


    // The sliders that show the weather (all but the switch and the button).
    const std::string* const VALUE_FIELDS[] =
    {
        &FIELD_RAIN, &FIELD_SNOW, &FIELD_WIND, &FIELD_WIND_DIR, &FIELD_WIND_VAR, &FIELD_GUST, &FIELD_WIND_DIR_VAR,
    };

    // "Import": the levels 0..3 of PlusRain / PlusSnow as the amounts that come back
    // as the same level (rainLevelOfMm(rainToMm(x)); 371 3.5).
    const F32 LEVEL_AMOUNT[4] = { 0.f, 0.15f, 0.4f, 0.8f };

    F32 amountOfLevel(S32 level)
    {
        return LEVEL_AMOUNT[llclamp(level, 0, 3)];
    }
}

LLPanelSettingsSkyWeatherTab::LLPanelSettingsSkyWeatherTab() :
    LLPanelSettingsSky(),
    mNoticeShown(0)
{
}

bool LLPanelSettingsSkyWeatherTab::postBuild()
{
    getChild<LLUICtrl>(FIELD_SWITCH)->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSwitchChanged(); });
    for (const std::string* field : VALUE_FIELDS)
    {
        getChild<LLUICtrl>(*field)->setCommitCallback([this](LLUICtrl*, const LLSD&) { onValueChanged(); });
    }
    getChild<LLUICtrl>(FIELD_IMPORT)->setCommitCallback([this](LLUICtrl*, const LLSD&) { onImportClicked(); });

    getChild<LLUICtrl>(FIELD_NOTICE)->setVisible(false);
    mNoticeShown = 0;
    refresh();
    return true;
}

void LLPanelSettingsSkyWeatherTab::draw()
{
    updateOwnNotice();
    LLPanelSettingsSky::draw();
}

void LLPanelSettingsSkyWeatherTab::refresh()
{
    if (!mSkySettings)
    {
        mBackupSky.reset();
        updateEnabled();
        return;
    }

    if (mBackupSky.lock() != mSkySettings)
    {
        // Another sky (another key frame of the day): its own values, not the last one's.
        mBackup = LLSkyWeather();
        mBackupSky = mSkySettings;
    }
    if (mSkySettings->hasWeather())
    {
        mBackup = mSkySettings->getWeather();
    }

    getChild<LLUICtrl>(FIELD_SWITCH)->setValue(mSkySettings->hasWeather());
    showValues(mBackup);
    updateEnabled();
}

void LLPanelSettingsSkyWeatherTab::updateEnabled()
{
    const bool can_edit = mSkySettings && getCanChangeSettings();
    const bool on = can_edit && mSkySettings->hasWeather();

    getChild<LLUICtrl>(FIELD_SWITCH)->setEnabled(can_edit);
    getChild<LLUICtrl>(FIELD_IMPORT)->setEnabled(on);
    for (const std::string* field : VALUE_FIELDS)
    {
        getChild<LLUICtrl>(*field)->setEnabled(on);
    }
}

void LLPanelSettingsSkyWeatherTab::updateOwnNotice()
{
    static LLCachedControl<S32> own_rain(gSavedSettings, SETTING_OWN_RAIN, 0);
    static LLCachedControl<S32> own_snow(gSavedSettings, SETTING_OWN_SNOW, 0);
    static LLCachedControl<bool> use_sky(gSavedSettings, SETTING_USE_SKY, true);

    // Own rain/snow wins over the sky's weather; with the photo tools' switch off the sky's is not used at all.
    const S32 state = (own_rain != 0 || own_snow != 0) ? 1 : (!use_sky ? 2 : 0);
    if (state != mNoticeShown)
    {
        mNoticeShown = state;
        LLUICtrl* notice = getChild<LLUICtrl>(FIELD_NOTICE);
        if (state != 0)
        {
            notice->setValue(getString(state == 1 ? "notice_own" : "notice_off"));
        }
        notice->setVisible(state != 0);
    }
}

void LLPanelSettingsSkyWeatherTab::showValues(const LLSkyWeather& w)
{
    getChild<LLUICtrl>(FIELD_RAIN)->setValue(w.rain * 100.f);
    getChild<LLUICtrl>(FIELD_SNOW)->setValue(w.snow * 100.f);
    getChild<LLUICtrl>(FIELD_WIND)->setValue(w.wind);
    getChild<LLUICtrl>(FIELD_WIND_DIR)->setValue(w.windDir);
    getChild<LLUICtrl>(FIELD_WIND_VAR)->setValue(w.windVar * 100.f);
    getChild<LLUICtrl>(FIELD_GUST)->setValue(w.gust * 100.f);
    getChild<LLUICtrl>(FIELD_WIND_DIR_VAR)->setValue(w.windDirVar);
    showHints(w);
}

void LLPanelSettingsSkyWeatherTab::showHints(const LLSkyWeather& w)
{
    LLStringUtil::format_map_t mm_args;
    mm_args["[MM]"] = llformat("%.1f", plus::slwx::tag::rainToMm(w.rain));
    LLUICtrl* rain_hint = getChild<LLUICtrl>(FIELD_RAIN_HINT);
    std::string text = getString("rain_hint");
    LLStringUtil::format(text, mm_args);
    if (rain_hint->getValue().asString() != text)
    {
        rain_hint->setValue(text);
    }

    LLStringUtil::format_map_t cm_args;
    cm_args["[CM]"] = llformat("%.1f", plus::slwx::tag::snowToCm(w.snow));
    LLUICtrl* snow_hint = getChild<LLUICtrl>(FIELD_SNOW_HINT);
    text = getString("snow_hint");
    LLStringUtil::format(text, cm_args);
    if (snow_hint->getValue().asString() != text)
    {
        snow_hint->setValue(text);
    }
}

LLSkyWeather LLPanelSettingsSkyWeatherTab::valuesFromControls() const
{
    LLSkyWeather w(mBackup);
    w.fromLand = false;     // what the user writes is the user's, even in a sky that came from the land
    // Not clamped here: the sliders hold the ranges, and setWeather() rounds into range (and
    // folds the wind direction into 0..360) whatever it is given, the import's values included.
    w.rain       = (F32)getChild<LLUICtrl>(FIELD_RAIN)->getValue().asReal() / 100.f;
    w.snow       = (F32)getChild<LLUICtrl>(FIELD_SNOW)->getValue().asReal() / 100.f;
    w.wind       = (F32)getChild<LLUICtrl>(FIELD_WIND)->getValue().asReal();
    w.windDir    = (F32)getChild<LLUICtrl>(FIELD_WIND_DIR)->getValue().asReal();
    w.windVar    = (F32)getChild<LLUICtrl>(FIELD_WIND_VAR)->getValue().asReal() / 100.f;
    w.gust       = (F32)getChild<LLUICtrl>(FIELD_GUST)->getValue().asReal() / 100.f;
    w.windDirVar = (F32)getChild<LLUICtrl>(FIELD_WIND_DIR_VAR)->getValue().asReal();
    return w;
}

void LLPanelSettingsSkyWeatherTab::onSwitchChanged()
{
    if (!mSkySettings)
    {
        return;
    }
    const bool on = getChild<LLUICtrl>(FIELD_SWITCH)->getValue().asBoolean();
    if (on)
    {
        mBackup.fromLand = false;                   // switching it on is the user's own act
        mSkySettings->setAssetId(LLUUID::null);     // no longer the asset it was read from
        mSkySettings->setWeather(mBackup);          // back with the values it had
    }
    else
    {
        if (mSkySettings->hasWeather())
        {
            mBackup = mSkySettings->getWeather();
        }
        mSkySettings->setAssetId(LLUUID::null);
        mSkySettings->clearWeather();               // the values stay in mBackup
    }
    mSkySettings->update();
    setIsDirty();
    updateEnabled();
}

void LLPanelSettingsSkyWeatherTab::onValueChanged()
{
    if (!mSkySettings || !mSkySettings->hasWeather())
    {
        return;
    }
    mBackup = valuesFromControls();
    mSkySettings->setAssetId(LLUUID::null);         // an edited sky is not its asset any more (kept whole, not by id, at login)
    mSkySettings->setWeather(mBackup);
    mSkySettings->update();
    showHints(mBackup);
    setIsDirty();
}

void LLPanelSettingsSkyWeatherTab::onImportClicked()
{
    if (!mSkySettings || !mSkySettings->hasWeather())
    {
        return;
    }

    LLSkyWeather w(mBackup);
    const S32 own_rain = gSavedSettings.getS32(SETTING_OWN_RAIN);
    const S32 own_snow = gSavedSettings.getS32(SETTING_OWN_SNOW);
    plus::slwx::Effective now;

    if (own_rain != 0 || own_snow != 0)
    {
        // The viewer's own weather (PlusRain, PlusSnow): snow wins over rain, as it does when drawn.
        w.snow = amountOfLevel(own_snow);
        w.rain = own_snow != 0 ? 0.f : amountOfLevel(own_rain);
        w.wind = gSavedSettings.getF32(SETTING_OWN_WIND);
        w.windDir = gSavedSettings.getF32(SETTING_OWN_WIND_DIR);
    }
    else if (plus::slwx::parcelEffective(now))
    {
        // No weather of one's own: the parcel's tag as it stands (not effective(): with the
        // plus/skyweather source in front, that would give back this sky's own weather).
        w.rain = plus::slwx::tag::mmToRain(now.rainMm);
        w.snow = amountOfLevel(now.snowLevel);
        w.wind = now.windBaseSpeed;
        w.windDir = now.windFromDeg;
    }
    else
    {
        // Nothing falls now: a calm start (wind as the settings have it).
        w.rain = 0.f;
        w.snow = 0.f;
        w.wind = gSavedSettings.getF32(SETTING_OWN_WIND);
        w.windDir = gSavedSettings.getF32(SETTING_OWN_WIND_DIR);
    }

    w.fromLand = false;                             // an import is the user's own weather
    mSkySettings->setAssetId(LLUUID::null);
    mSkySettings->setWeather(w);
    mSkySettings->update();
    mBackup = mSkySettings->getWeather();
    showValues(mBackup);
    setIsDirty();
}
// </plus> skyweather-ui
