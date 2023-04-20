/* esp32-firmware
 * Copyright (C) 2020-2021 Erik Fleckstein <erik@tinkerforge.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "meter.h"

#include "api.h"
#include "event_log.h"
#include "tools.h"
#include "task_scheduler.h"
#include "modules.h"


void Meter::pre_setup()
{
    state = Config::Object({
        {"state", Config::Uint8(0)}, // 0 - no energy meter, 1 - initialization error, 2 - meter available
        {"type", Config::Uint8(0)} // 0 - not available, 1 - sdm72, 2 - sdm630, 3 - sdm72v2, 4 - MQTT meter
    });

    values = Config::Object({
        {"power", Config::Float(NAN)},
        {"energy_rel", Config::Float(NAN)},
        {"energy_abs", Config::Float(NAN)},
    });

    phases = Config::Object({
        {"phases_connected", Config::Array({Config::Bool(false),Config::Bool(false),Config::Bool(false)},
            new Config{Config::Bool(false)},
            3, 3, Config::type_id<Config::ConfBool>())},
        {"phases_active", Config::Array({Config::Bool(false),Config::Bool(false),Config::Bool(false)},
            new Config{Config::Bool(false)},
            3, 3, Config::type_id<Config::ConfBool>())}
    });

    all_values = Config::Array({},
        new Config{Config::Float(NAN)},
        0, METER_ALL_VALUES_COUNT, Config::type_id<Config::ConfFloat>());

    last_reset = Config::Object({
        {"last_reset", Config::Uint32(0)}
    });
}

void Meter::updateMeterState(uint8_t new_state, uint8_t new_type)
{
    state.get("state")->updateUint(new_state);
    state.get("type")->updateUint(new_type);

    if (new_state == 2) {
        this->setupMeter(new_type);
    }
}

void Meter::updateMeterState(uint8_t new_state)
{
    state.get("state")->updateUint(new_state);

    if (new_state == 2) {
        this->setupMeter(state.get("type")->asUint());
    }
}

void Meter::updateMeterType(uint8_t new_type)
{
    state.get("type")->updateUint(new_type);
}

void Meter::updateMeterValues(float power, float energy_rel, float energy_abs)
{
    if (!meter_setup_done)
        return;
    bool changed = false;
    float old_value;

    if (!isnan(power)) {
        old_value = values.get("power")->asFloat();
        changed |= values.get("power")->updateFloat(power) && !isnanf(old_value);
    }

    if (!isnan(energy_rel)) {
        old_value = values.get("energy_rel")->asFloat();
        changed |= values.get("energy_rel")->updateFloat(energy_rel) && !isnanf(old_value);
    }

    if (!isnan(energy_abs)) {
        old_value = values.get("energy_abs")->asFloat();
        changed |= values.get("energy_abs")->updateFloat(energy_abs) && !isnanf(old_value);
    }

    if (changed)
        last_value_change = esp_timer_get_time();

    power_hist.add_sample(power);
}

void Meter::updateMeterPhases(bool phases_connected[3], bool phases_active[3])
{
    if (!meter_setup_done)
        return;

    for (int i = 0; i < 3; ++i)
        phases.get("phases_active")->get(i)->updateBool(phases_active[i]);

    for (int i = 0; i < 3; ++i)
        phases.get("phases_connected")->get(i)->updateBool(phases_connected[i]);
}

void Meter::updateMeterAllValues(int idx, float val)
{
    if (!meter_setup_done)
        return;

    all_values.get(idx)->updateFloat(val);
}

void Meter::updateMeterAllValues(float values[METER_ALL_VALUES_COUNT])
{
    if (!meter_setup_done)
        return;

    bool changed = false;

    for (int i = 0; i < METER_ALL_VALUES_COUNT; ++i)
        if (!isnan(values[i])) {
            auto wrap = all_values.get(i);
            auto old_value = wrap->asFloat();
            changed |= wrap->updateFloat(values[i]) && !isnanf(old_value);
        }

    if (changed) {
        last_value_change = esp_timer_get_time();
    }
}

void Meter::registerResetCallback(std::function<void(void)> cb)
{
    this->reset_callbacks.push_back(cb);
}

void Meter::setupMeter(uint8_t meter_type)
{
    if (meter_setup_done)
        return;

    api.addFeature("meter");
    switch(meter_type) {
        case METER_TYPE_SDM630:
        case METER_TYPE_SDM72DMV2:
        case METER_TYPE_SDM630MCTV2:
            api.addFeature("meter_phases");
            /* FALLTHROUGH*/
        case METER_TYPE_SDM72CTM:
            api.addFeature("meter_all_values");
            break;
    }

    for (int i = all_values.count(); i < METER_ALL_VALUES_COUNT; ++i) {
        all_values.add();
    }

    meter_setup_done = true;
}

void Meter::setup()
{
    initialized = true;
    api.restorePersistentConfig("meter/last_reset", &last_reset);
    power_hist.setup();
}

void Meter::register_urls()
{
    api.addState("meter/state", &state, {}, 1000);
    api.addState("meter/values", &values, {}, 1000);
    api.addState("meter/phases", &phases, {}, 1000);
    api.addState("meter/all_values", &all_values, {}, 1000);
    api.addState("meter/last_reset", &last_reset, {}, 1000);

    api.addCommand("meter/reset", Config::Null(), {}, [this](){
        for (auto &cb : this->reset_callbacks)
            cb();

        struct timeval tv_now;

        if (clock_synced(&tv_now)) {
            last_reset.get("last_reset")->updateUint(tv_now.tv_sec);
        } else {
            last_reset.get("last_reset")->updateUint(0);
        }
        api.writeConfig("meter/last_reset", &last_reset);
    }, true);

#if MODULE_EVSE_V2_AVAILABLE() || MODULE_EVSE_AVAILABLE()
    #if MODULE_EVSE_V2_AVAILABLE()
        if (evse_v2.get_require_meter_enabled()) {
    #elif MODULE_EVSE_AVAILABLE()
        if (evse.get_require_meter_enabled()) {
    #endif
            last_value_change = -METER_TIMEOUT_US - 1;
            task_scheduler.scheduleWithFixedDelay([this]() {
                int64_t now = esp_timer_get_time();

                bool meter_timeout = state.get("state")->asUint() != 2 || now - last_value_change > METER_TIMEOUT_US;

                #if MODULE_EVSE_V2_AVAILABLE()
                    evse_v2.set_require_meter_blocking(meter_timeout);
                #elif MODULE_EVSE_AVAILABLE()
                    evse.set_require_meter_blocking(meter_timeout);
                #endif

                if (meter_timeout)
                    users.stop_charging(0, true, 0);

            }, 0, 1000);
    }
#endif

    power_hist.register_urls("meter/");
}
