/**
 * @file skyweatherpanel.h
 * @brief The weather tab of the sky editors (research/371-weather-tab.md 3.3)
 *
 * One panel for both windows that edit a sky: the fixed-environment sky window
 * (LLFloaterFixedEnvironmentSky) and the day cycle editor's sky tabs. It
 * writes the sky's own weather (LLSkyWeather, llinventory/llskyweather.h).
 * The switch at the top makes the sky decide the weather (setWeather) or not
 * (clearWeather: the values stay in mBackup and come back when it is switched
 * on again).
 */

#ifndef PLUS_SKYWEATHERPANEL_H
#define PLUS_SKYWEATHERPANEL_H

#include "llpaneleditsky.h"
#include "llskyweather.h"

#include <memory>

// <plus> skyweather-ui
class LLPanelSettingsSkyWeatherTab : public LLPanelSettingsSky
{
    LOG_CLASS(LLPanelSettingsSkyWeatherTab);

public:
                            LLPanelSettingsSkyWeatherTab();

    bool                    postBuild() override;
    void                    draw() override;

protected:
    void                    refresh() override;

private:
    void                    onSwitchChanged();
    void                    onValueChanged();
    void                    onImportClicked();

    LLSkyWeather            valuesFromControls() const;     // mBackup with the sliders' values put in
    void                    showValues(const LLSkyWeather& w);
    void                    showHints(const LLSkyWeather& w);
    void                    updateEnabled();
    void                    updateOwnNotice();

    LLSkyWeather            mBackup;        // what the sliders show; kept while the switch is off
    std::weak_ptr<LLSettingsSky> mBackupSky; // the sky mBackup belongs to
    S32                     mNoticeShown;           // 0 none, 1 own rain/snow wins, 2 sky weather is off
};
// </plus> skyweather-ui

#endif // PLUS_SKYWEATHERPANEL_H
