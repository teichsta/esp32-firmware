/* esp32-firmware
 * Copyright (C) 2023 Matthias Bolte <matthias@tinkerforge.com>
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

#include "energy_manager.h"

#include <Arduino.h>
#include <sys/time.h>
#include <time.h>
#include "event_log.h"
#include "modules.h"

#define MAX_DATA_AGE 30000 // milliseconds
#define DATA_INTERVAL_5MIN 5 // minutes

void EnergyManager::collect_data_points()
{
    struct timeval tv;

    if (!clock_synced(&tv)) {
        return;
    }

    struct tm utc;
    gmtime_r(&tv.tv_sec, &utc);

    int current_5min_slot = utc.tm_min / 5;

    if (current_5min_slot != last_history_5min_slot) {
        for (const auto &charger : charge_manager.charge_manager_state.get("chargers")) {
            uint32_t last_update = charger.get("last_update")->asUint();

            if (!deadline_elapsed(last_update + MAX_DATA_AGE)) {
                uint8_t charger_state = charger.get("charger_state")->asUint();

                uint32_t uid = charger.get("uid")->asUint();
                uint8_t flags = charger_state; // bit 0-2 = charger state, bit 7 = no data (read only)
                uint16_t power = UINT16_MAX;

                if (charger.get("meter_supported")->asBool()) {
                    power = clamp((uint64_t)0, (uint64_t)roundf(charger.get("power_total")->asFloat()), (uint64_t)UINT16_MAX - 1); // W
                }

                set_wallbox_5min_data_point(&utc, uid, flags, power);
            }
        }

        if (all_data.is_valid && !deadline_elapsed(all_data.last_update + MAX_DATA_AGE)) {
            uint8_t flags = 0; // bit 0 = 1p/3p, bit 1-2 = input, bit 3 = output, bit 7 = no data (read only)
            int32_t power_grid = INT32_MAX; // W
            int32_t power_general[6] = {INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX}; // W

            flags |= is_3phase         ? 0b0001 : 0;
            flags |= all_data.input[0] ? 0b0010 : 0;
            flags |= all_data.input[1] ? 0b0100 : 0;
            flags |= all_data.output   ? 0b1000 : 0;

            if (meter.state.get("state")->asUint() == 2) {
                power_grid = clamp((int64_t)INT32_MIN, (int64_t)roundf(meter.values.get("power")->asFloat()), (int64_t)INT32_MAX - 1); // W
            }

            // FIXME: fill power_general

            set_energy_manager_5min_data_point(&utc, flags, power_grid, power_general);
        }

        last_history_5min_slot = current_5min_slot;
    }

    struct tm local;
    localtime_r(&tv.tv_sec, &local);

    int current_daily_slot = local.tm_year * 366 + local.tm_yday;

    if (current_daily_slot != last_history_daily_slot && local.tm_hour == 23 && local.tm_min >= 55) {
        for (const auto &charger : charge_manager.charge_manager_state.get("chargers")) {
            uint32_t last_update = charger.get("last_update")->asUint();

            if (!deadline_elapsed(last_update + MAX_DATA_AGE)) {
                uint32_t uid = charger.get("uid")->asUint();
                uint32_t energy = UINT32_MAX;

                if (charger.get("meter_supported")->asBool()) {
                    energy = clamp((uint64_t)0, (uint64_t)roundf(charger.get("energy_abs")->asFloat() * 100.0), (uint64_t)UINT32_MAX - 1); // kWh -> dWh
                }

                set_wallbox_daily_data_point(&local, uid, energy);
            }
        }

        if (all_data.is_valid && !deadline_elapsed(all_data.last_update + MAX_DATA_AGE)) {
            uint32_t energy_grid_in = UINT32_MAX; // dWh
            uint32_t energy_grid_out = UINT32_MAX; // dWh
            uint32_t energy_general_in[6] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}; // dWh
            uint32_t energy_general_out[6] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}; // dWh

            if (meter.state.get("state")->asUint() == 2 && api.hasFeature("meter_all_values")) {
                energy_grid_in = clamp((uint64_t)0, (uint64_t)roundf(meter.all_values.get(METER_ALL_VALUES_TOTAL_IMPORT_KWH)->asFloat() * 100.0), (uint64_t)UINT32_MAX - 1); // kWh -> dWh
                energy_grid_out = clamp((uint64_t)0, (uint64_t)roundf(meter.all_values.get(METER_ALL_VALUES_TOTAL_EXPORT_KWH)->asFloat() * 100.0), (uint64_t)UINT32_MAX - 1); // kWh -> dWh
            }

            // FIXME: fill energy_general_in and energy_general_out

            set_energy_manager_daily_data_point(&local, energy_grid_in, energy_grid_out, energy_general_in, energy_general_out);
        }

        last_history_daily_slot = current_daily_slot;
    }
}

void EnergyManager::set_wallbox_5min_data_point(struct tm *utc, uint32_t uid, uint8_t flags, uint16_t power /* W */)
{
    uint8_t status;
    uint8_t year = utc->tm_year - 100;
    uint8_t month = utc->tm_mon + 1;
    uint8_t day = utc->tm_mday;
    uint8_t hour = utc->tm_hour;
    uint8_t minute = (utc->tm_min / 5) * 5;
    int rc = tf_warp_energy_manager_set_sd_wallbox_data_point(&device, uid, year, month, day, hour, minute, flags, power, &status);

    check_bricklet_reachable(rc, "set_wallbox_5min_data_point");

    //logger.printfln("set_wallbox_5min_data_point: u%u %d-%02d-%02d %02d:%02d f%u p%u",
    //                uid, 2000 + year, month, day, hour, minute, flags, power);

    if (rc != TF_E_OK) {
        logger.printfln("energy_manager: Failed to set wallbox 5min data point: error %d", rc);
    }
    else if (status != 0) {
        logger.printfln("energy_manager: Failed to set wallbox 5min data point: status %u", status);
    }
    else {
        char *buf;
        int buf_written = asprintf(&buf, "{\"topic\":\"energy_manager/history_wallbox_5min_changed\",\"payload\":{\"uid\":%u,\"year\":%u,\"month\":%u,\"day\":%u,\"hour\":%u,\"minute\":%u,\"flags\":%u,\"power\":%u}}\n",
                                   uid, 2000 + year, month, day, hour, minute, flags, power);

        if (buf_written > 0) {
            ws.web_sockets.sendToAllOwned(buf, buf_written);
        }
    }
}

void EnergyManager::set_wallbox_daily_data_point(struct tm *local, uint32_t uid, uint32_t energy /* dWh */)
{
    uint8_t status;
    uint8_t year = local->tm_year - 100;
    uint8_t month = local->tm_mon + 1;
    uint8_t day = local->tm_mday;
    int rc = tf_warp_energy_manager_set_sd_wallbox_daily_data_point(&device, uid, year, month, day, energy, &status);

    check_bricklet_reachable(rc, "set_wallbox_daily_data_point");

    //logger.printfln("set_wallbox_daily_data_point: u%u %d-%02d-%02d e%u",
    //                uid, 2000 + year, month, day, energy);

    if (rc != TF_E_OK) {
        logger.printfln("energy_manager: Failed to set wallbox daily data point: error %d", rc);
    }
    else if (status != 0) {
        logger.printfln("energy_manager: Failed to set wallbox daily data point: status %u", status);
    }
    else {
        char *buf;
        int buf_written = asprintf(&buf, "{\"topic\":\"energy_manager/history_wallbox_daily_changed\",\"payload\":{\"uid\":%u,\"year\":%u,\"month\":%u,\"day\":%u,\"energy\":%.2f}}\n",
                                   uid, 2000 + year, month, day, energy / 100.0);

        if (buf_written > 0) {
            ws.web_sockets.sendToAllOwned(buf, buf_written);
        }
    }
}

void EnergyManager::set_energy_manager_5min_data_point(struct tm *utc, uint8_t flags, int32_t power_grid /* W */, int32_t power_general[6] /* W */)
{
    uint8_t status;
    uint8_t year = utc->tm_year - 100;
    uint8_t month = utc->tm_mon + 1;
    uint8_t day = utc->tm_mday;
    uint8_t hour = utc->tm_hour;
    uint8_t minute = (utc->tm_min / 5) * 5;
    int rc = tf_warp_energy_manager_set_sd_energy_manager_data_point(&device, year, month, day, hour, minute, flags, power_grid, power_general, &status);

    check_bricklet_reachable(rc, "set_energy_manager_5min_data_point");

    //logger.printfln("set_energy_manager_5min_data_point: %d-%02d-%02d %02d:%02d f%u gr%d ge%d,%d,%d,%d,%d,%d",
    //                2000 + year, month, day, hour, minute, flags, power_grid, power_general[0], power_general[1], power_general[2], power_general[3], power_general[4], power_general[5]);

    if (rc != TF_E_OK) {
        logger.printfln("energy_manager: Failed to set energy manager 5min data point: error %d", rc);
    }
    else if (status != 0) {
        logger.printfln("energy_manager: Failed to set energy manager 5min data point: status %u", status);
    }
    else {
        char *buf;
        int buf_written = asprintf(&buf, "{\"topic\":\"energy_manager/history_energy_manager_5min_changed\",\"payload\":{\"year\":%u,\"month\":%u,\"day\":%u,\"hour\":%u,\"minute\":%u,\"flags\":%u,\"power_grid\":%d,\"power_general\":[%d,%d,%d,%d,%d,%d]}}\n",
                                   2000 + year, month, day, hour, minute, flags, power_grid, power_general[0], power_general[1], power_general[2], power_general[3], power_general[4], power_general[5]);

        if (buf_written > 0) {
            ws.web_sockets.sendToAllOwned(buf, buf_written);
        }
    }
}

void EnergyManager::set_energy_manager_daily_data_point(struct tm *local,
                                                        uint32_t energy_grid_in /* dWh */,
                                                        uint32_t energy_grid_out /* dWh */,
                                                        uint32_t energy_general_in[6] /* dWh */,
                                                        uint32_t energy_general_out[6] /* dWh */)
{
    uint8_t status;
    uint8_t year = local->tm_year - 100;
    uint8_t month = local->tm_mon + 1;
    uint8_t day = local->tm_mday;
    int rc = tf_warp_energy_manager_set_sd_energy_manager_daily_data_point(&device,
                                                                           year,
                                                                           month,
                                                                           day,
                                                                           energy_grid_in,
                                                                           energy_grid_out,
                                                                           energy_general_in,
                                                                           energy_general_out,
                                                                           &status);

    check_bricklet_reachable(rc, "set_energy_manager_daily_data_point");

    //logger.printfln("set_energy_manager_daily_data_point: %d-%02d-%02d gri%u gro%u gei%u,%u,%u,%u,%u,%u geo%u,%u,%u,%u,%u,%u",
    //                2000 + year, month, day,
    //                energy_grid_in, energy_grid_out,
    //                energy_general_in[0], energy_general_in[1], energy_general_in[2], energy_general_in[3], energy_general_in[4], energy_general_in[5],
    //                energy_general_out[0], energy_general_out[1], energy_general_out[2], energy_general_out[3], energy_general_out[4], energy_general_out[5]);

    if (rc != TF_E_OK) {
        logger.printfln("energy_manager: Failed to set energy manager daily data point: error %i", rc);
    }
    else if (status != 0) {
        logger.printfln("energy_manager: Failed to set energy manager daily data point: status %u", status);
    } else {
        char *buf;
        int buf_written = asprintf(&buf, "{\"topic\":\"energy_manager/history_energy_manager_daily_changed\",\"payload\":{\"year\":%u,\"month\":%u,\"day\":%u,\"energy_grid_in\":%.2f,\"energy_grid_out\":%.2f,\"energy_general_in\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f],\"energy_general_out\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f]}}\n",
                                   2000 + year, month, day, energy_grid_in / 100.0, energy_grid_out / 100.0,
                                   energy_general_in[0] / 100.0, energy_general_in[1] / 100.0, energy_general_in[2] / 100.0, energy_general_in[3] / 100.0, energy_general_in[4] / 100.0, energy_general_in[5] / 100.0,
                                   energy_general_out[0] / 100.0, energy_general_out[1] / 100.0, energy_general_out[2] / 100.0, energy_general_out[3] / 100.0, energy_general_out[4] / 100.0, energy_general_out[5] / 100.0);

        if (buf_written > 0) {
            ws.web_sockets.sendToAllOwned(buf, buf_written);
        }
    }
}

typedef struct {
    IChunkedResponse *response;
    Ownership *response_ownership;
    uint32_t response_owner_id;
    bool call_begin;
    bool write_comma;
    uint16_t next_offset;
} StreamMetadata;

static StreamMetadata metadata_array[4];

typedef struct {
    uint8_t flags; // bit 0-2 = charger state, bit 7 = no data (read only)
    uint16_t power; // W
} __attribute__((__packed__)) Wallbox5minData;

static void wallbox_5min_data_points_handler(TF_WARPEnergyManager *device, uint16_t data_length, uint16_t data_chunk_offset, uint8_t data_chunk_data[60], void *user_data)
{
    StreamMetadata *metadata = (StreamMetadata *)user_data;
    IChunkedResponse *response = metadata->response;
    bool write_success = true;
    OwnershipGuard ownership_guard(metadata->response_ownership, metadata->response_owner_id);

    if (!ownership_guard.have_ownership()) {
        tf_warp_energy_manager_register_sd_wallbox_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    if (metadata->call_begin) {
        metadata->call_begin = false;

        response->begin(true);

        if (write_success) {
            write_success = response->writen("[");
        }
    }

    if (metadata->next_offset != data_chunk_offset) {
        logger.printfln("energy_manager: Failed to get wallbox 5min data point: stream out of sync (%u != %u)", metadata->next_offset, data_chunk_offset);

        if (write_success) {
            write_success = response->writen("]");
        }

        write_success &= response->flush();
        response->end(write_success ? "" : "write error");

        tf_warp_energy_manager_register_sd_wallbox_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    uint16_t actual_length = data_length - data_chunk_offset;
    uint16_t i;
    Wallbox5minData *p;

    if (actual_length > 60) {
        actual_length = 60;
    }

    if (metadata->write_comma && write_success) {
        write_success = response->writen(",");
    }

    for (i = 0; i < actual_length && write_success; i += sizeof(Wallbox5minData)) {
        p = (Wallbox5minData *)&data_chunk_data[i];

        if ((p->flags & 0x80 /* no data */) != 0) {
            p->power = UINT16_MAX;
        }

        write_success = response->writef("%u,", p->flags);

        if (write_success) {
            if (p->power != UINT16_MAX) {
                write_success = response->writef("%u", p->power);
            } else {
                write_success = response->writef("null");
            }

            if (write_success && i < actual_length - sizeof(Wallbox5minData)) {
                write_success = response->writen(",");
            }
        }
    }

    metadata->write_comma = true;
    metadata->next_offset += 60;

    if (metadata->next_offset >= data_length) {
        if (write_success) {
            write_success = response->writen("]");
        }

        write_success &= response->flush();
        response->end(write_success ? "" : "write error");

        tf_warp_energy_manager_register_sd_wallbox_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    if (!write_success) {
        response->end("write error");

        tf_warp_energy_manager_register_sd_wallbox_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }
}

void EnergyManager::history_wallbox_5min_response(IChunkedResponse *response, Ownership *response_ownership, uint32_t response_owner_id)
{
    uint32_t uid = history_wallbox_5min.get("uid")->asUint();

    // date in UTC to avoid DST overlap problems
    uint8_t year = history_wallbox_5min.get("year")->asUint() - 2000;
    uint8_t month = history_wallbox_5min.get("month")->asUint();
    uint8_t day = history_wallbox_5min.get("day")->asUint();

    uint8_t status;
    int rc = tf_warp_energy_manager_get_sd_wallbox_data_points(&device, uid, year, month, day, 0, 0, 288, &status);

    //logger.printfln("history_wallbox_5min_response: u%u %d-%02d-%02d",
    //                uid, 2000 + year, month, day);

    if (rc != TF_E_OK || status != 0) {
        OwnershipGuard ownership_guard(response_ownership, response_owner_id);

        if (ownership_guard.have_ownership()) {
            response->begin(false);

            if (rc != TF_E_OK) {
                response->writef("Failed to get wallbox 5min data point: error %d", rc);
                logger.printfln("energy_manager: Failed to get wallbox 5min data point: error %d", rc);
            }
            else if (status != 0) {
                response->writef("Failed to get wallbox 5min data point: status %u", status);
                logger.printfln("energy_manager: Failed to get wallbox 5min data point: status %u", status);
            }

            response->flush();
            response->end("");
        }
    }
    else {
        StreamMetadata *metadata = &metadata_array[0];

        metadata->response = response;
        metadata->response_ownership = response_ownership;
        metadata->response_owner_id = response_owner_id;
        metadata->call_begin = true;
        metadata->write_comma = false;
        metadata->next_offset = 0;

        tf_warp_energy_manager_register_sd_wallbox_data_points_low_level_callback(&device, wallbox_5min_data_points_handler, metadata);
    }

    check_bricklet_reachable(rc, "history_wallbox_5min_response");
}

static void wallbox_daily_data_points_handler(TF_WARPEnergyManager *device,
                                              uint16_t data_length,
                                              uint16_t data_chunk_offset,
                                              uint32_t data_chunk_data[15],
                                              void *user_data)
{
    StreamMetadata *metadata = (StreamMetadata *)user_data;
    IChunkedResponse *response = metadata->response;
    bool write_success = true;
    OwnershipGuard ownership_guard(metadata->response_ownership, metadata->response_owner_id);

    if (!ownership_guard.have_ownership()) {
        tf_warp_energy_manager_register_sd_wallbox_daily_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    if (metadata->call_begin) {
        metadata->call_begin = false;

        response->begin(true);

        if (write_success) {
            write_success = response->writen("[");
        }
    }

    if (metadata->next_offset != data_chunk_offset) {
        logger.printfln("energy_manager: Failed to get wallbox daily data point: stream out of sync (%u != %u)",
                        metadata->next_offset, data_chunk_offset);

        if (write_success) {
            write_success = response->writen("]");
        }

        write_success &= response->flush();
        response->end(write_success ? "" : "write error");

        tf_warp_energy_manager_register_sd_wallbox_daily_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    uint16_t actual_length = data_length - data_chunk_offset;
    uint16_t i;

    if (actual_length > 15) {
        actual_length = 15;
    }

    if (metadata->write_comma && write_success) {
        write_success = response->writen(",");
    }

    for (i = 0; i < actual_length && write_success; ++i) {
        if (data_chunk_data[i] != UINT32_MAX) {
            write_success = response->writef("%.2f", data_chunk_data[i] / 100.0); // dWh -> kWh
        } else {
            write_success = response->writen("null");
        }

        if (write_success && i < actual_length - 1) {
            write_success = response->writen(",");
        }
    }

    metadata->write_comma = true;
    metadata->next_offset += 15;

    if (metadata->next_offset >= data_length) {
        if (write_success) {
            write_success = response->writen("]");
        }

        write_success &= response->flush();
        response->end(write_success ? "" : "write error");

        tf_warp_energy_manager_register_sd_wallbox_daily_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    if (!write_success) {
        response->end("write error");

        tf_warp_energy_manager_register_sd_wallbox_daily_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }
}

void EnergyManager::history_wallbox_daily_response(IChunkedResponse *response,
                                                   Ownership *response_ownership,
                                                   uint32_t response_owner_id)
{
    uint32_t uid = history_wallbox_daily.get("uid")->asUint();

    // date in local time to have the days properly aligned
    uint8_t year = history_wallbox_daily.get("year")->asUint() - 2000;
    uint8_t month = history_wallbox_daily.get("month")->asUint();

    uint8_t status;
    int rc = tf_warp_energy_manager_get_sd_wallbox_daily_data_points(&device, uid, year, month, 1, 31, &status);

    //logger.printfln("history_wallbox_daily_response: u%u %d-%02d",
    //                uid, 2000 + year, month);

    if (rc != TF_E_OK || status != 0) {
        OwnershipGuard ownership_guard(response_ownership, response_owner_id);

        if (ownership_guard.have_ownership()) {
            response->begin(false);

            if (rc != TF_E_OK) {
                response->writef("Failed to get wallbox daily data point: error %d", rc);
                logger.printfln("energy_manager: Failed to get wallbox daily data point: error %d", rc);
            }
            else if (status != 0) {
                response->writef("Failed to get wallbox daily data point: status %u", status);
                logger.printfln("energy_manager: Failed to get wallbox daily data point: status %u", status);
            }

            response->flush();
            response->end("");
        }
    }
    else {
        StreamMetadata *metadata = &metadata_array[1];

        metadata->response = response;
        metadata->response_ownership = response_ownership;
        metadata->response_owner_id = response_owner_id;
        metadata->call_begin = true;
        metadata->write_comma = false;
        metadata->next_offset = 0;

        tf_warp_energy_manager_register_sd_wallbox_daily_data_points_low_level_callback(&device, wallbox_daily_data_points_handler, metadata);
    }

    check_bricklet_reachable(rc, "history_wallbox_daily_response");
}

typedef struct {
    uint8_t flags; // bit 0 = 1p/3p, bit 1-2 = input, bit 3 = output, bit 7 = no data
    int32_t power_grid; // W
    int32_t power_general[6]; // W
} __attribute__((__packed__)) EnergyManager5MinData;

static void energy_manager_5min_data_points_handler(TF_WARPEnergyManager *device,
                                                    uint16_t data_length,
                                                    uint16_t data_chunk_offset,
                                                    uint8_t data_chunk_data[58],
                                                    void *user_data)
{
    StreamMetadata *metadata = (StreamMetadata *)user_data;
    IChunkedResponse *response = metadata->response;
    bool write_success = true;
    OwnershipGuard ownership_guard(metadata->response_ownership, metadata->response_owner_id);

    if (!ownership_guard.have_ownership()) {
        tf_warp_energy_manager_register_sd_energy_manager_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    if (metadata->call_begin) {
        metadata->call_begin = false;

        response->begin(true);

        if (write_success) {
            write_success = response->writen("[");
        }
    }

    if (metadata->next_offset != data_chunk_offset) {
        logger.printfln("energy_manager: Failed to get energy manager 5min data point: stream out of sync (%u != %u)",
                        metadata->next_offset, data_chunk_offset);

        if (write_success) {
            write_success = response->writen("]");
        }

        write_success &= response->flush();
        response->end(write_success ? "" : "write error");

        tf_warp_energy_manager_register_sd_energy_manager_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    uint16_t actual_length = data_length - data_chunk_offset;
    uint16_t i;
    EnergyManager5MinData *p;

    if (actual_length > 58) {
        actual_length = 58;
    }

    if (metadata->write_comma && write_success) {
        write_success = response->write(",", 1);
    }

    for (i = 0; i < actual_length && write_success; i += sizeof(EnergyManager5MinData)) {
        p = (EnergyManager5MinData *)&data_chunk_data[i];

        if ((p->flags & 0x80 /* no data */) != 0) {
            p->power_grid = INT32_MAX;

            for (int k = 0; k < 6; ++k) {
                p->power_general[k] = INT32_MAX;
            }
        }

        write_success = response->writef("%u,", p->flags);

        if (write_success) {
            if (p->power_grid != INT32_MAX) {
                write_success = response->writef("%d", p->power_grid);
            } else {
                write_success = response->writef("null");
            }

            for (int k = 0; k < 6 && write_success; ++k) {
                if (p->power_general[k] != INT32_MAX) {
                    write_success = response->writef(",%d", p->power_general[k]);
                } else {
                    write_success = response->writef(",null");
                }
            }

            if (write_success && i < actual_length - sizeof(EnergyManager5MinData)) {
                write_success = response->writen(",");
            }
        }
    }

    metadata->write_comma = true;
    metadata->next_offset += 58;

    if (metadata->next_offset >= data_length) {
        if (write_success) {
            write_success = response->writen("]");
        }

        write_success &= response->flush();
        response->end(write_success ? "" : "write error");

        tf_warp_energy_manager_register_sd_energy_manager_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    if (!write_success) {
        response->end("write error");

        tf_warp_energy_manager_register_sd_energy_manager_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }
}

void EnergyManager::history_energy_manager_5min_response(IChunkedResponse *response,
                                                         Ownership *response_ownership,
                                                         uint32_t response_owner_id)
{
    // date in UTC to avoid DST overlap problems
    uint8_t year = history_energy_manager_5min.get("year")->asUint() - 2000;
    uint8_t month = history_energy_manager_5min.get("month")->asUint();
    uint8_t day = history_energy_manager_5min.get("day")->asUint();

    uint8_t status;
    int rc = tf_warp_energy_manager_get_sd_energy_manager_data_points(&device, year, month, day, 0, 0, 288, &status);

    //logger.printfln("history_energy_manager_5min_response: %d-%02d-%02d",
    //                2000 + year, month, day);

    if (rc != TF_E_OK || status != 0) {
        OwnershipGuard ownership_guard(response_ownership, response_owner_id);

        if (ownership_guard.have_ownership()) {
            response->begin(false);

            if (rc != TF_E_OK) {
                response->writef("Failed to get energy manager 5min data point: error %d", rc);
                logger.printfln("energy_manager: Failed to get energy manager 5min data point: error %d", rc);
            }
            else if (status != 0) {
                response->writef("Failed to get energy manager 5min data point: status %u", status);
                logger.printfln("energy_manager: Failed to get energy manager 5min data point: status %u", status);
            }

            response->flush();
            response->end("");
        }
    }
    else {
        StreamMetadata *metadata = &metadata_array[2];

        metadata->response = response;
        metadata->response_ownership = response_ownership;
        metadata->response_owner_id = response_owner_id;
        metadata->call_begin = true;
        metadata->write_comma = false;
        metadata->next_offset = 0;

        tf_warp_energy_manager_register_sd_energy_manager_data_points_low_level_callback(&device, energy_manager_5min_data_points_handler, metadata);
    }

    check_bricklet_reachable(rc, "history_energy_manager_5min_response");
}

static void energy_manager_daily_data_points_handler(TF_WARPEnergyManager *device,
                                                     uint16_t data_length,
                                                     uint16_t data_chunk_offset,
                                                     uint32_t data_chunk_data[14],
                                                     void *user_data)
{
    StreamMetadata *metadata = (StreamMetadata *)user_data;
    IChunkedResponse *response = metadata->response;
    bool write_success = true;
    OwnershipGuard ownership_guard(metadata->response_ownership, metadata->response_owner_id);

    if (!ownership_guard.have_ownership()) {
        tf_warp_energy_manager_register_sd_energy_manager_daily_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    if (metadata->call_begin) {
        metadata->call_begin = false;

        response->begin(true);

        if (write_success) {
            write_success = response->writen("[");
        }
    }

    if (metadata->next_offset != data_chunk_offset) {
        logger.printfln("energy_manager: Failed to get energy manager daily data point: stream out of sync (%u != %u)",
                        metadata->next_offset, data_chunk_offset);

        if (write_success) {
            write_success = response->writen("]");
        }

        write_success &= response->flush();
        response->end(write_success ? "" : "write error");

        tf_warp_energy_manager_register_sd_energy_manager_daily_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    uint16_t actual_length = data_length - data_chunk_offset;
    uint16_t i;

    if (actual_length > 14) {
        actual_length = 14;
    }

    if (metadata->write_comma && write_success) {
        write_success = response->writen(",");
    }

    for (i = 0; i < actual_length && write_success; ++i) {
        if (data_chunk_data[i] != UINT32_MAX) {
            write_success = response->writef("%.2f", data_chunk_data[i] / 100.0); // dWh -> kWh
        } else {
            write_success = response->writen("null");
        }

        if (write_success && i < actual_length - 1) {
            write_success = response->writen(",");
        }
    }

    metadata->write_comma = true;
    metadata->next_offset += 14;

    if (metadata->next_offset >= data_length) {
        if (write_success) {
            write_success = response->writen("]");
        }

        write_success &= response->flush();
        response->end(write_success ? "" : "write error");

        tf_warp_energy_manager_register_sd_energy_manager_daily_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }

    if (!write_success) {
        response->end("write error");

        tf_warp_energy_manager_register_sd_energy_manager_daily_data_points_low_level_callback(device, nullptr, nullptr);
        return;
    }
}

void EnergyManager::history_energy_manager_daily_response(IChunkedResponse *response,
                                                          Ownership *response_ownership,
                                                          uint32_t response_owner_id)
{
    // date in local time to have the days properly aligned
    uint8_t year = history_energy_manager_daily.get("year")->asUint() - 2000;
    uint8_t month = history_energy_manager_daily.get("month")->asUint();

    uint8_t status;
    int rc = tf_warp_energy_manager_get_sd_energy_manager_daily_data_points(&device, year, month, 1, 31, &status);

    //logger.printfln("history_energy_manager_daily_response: %d-%02d",
    //                2000 + year, month);

    if (rc != TF_E_OK || status != 0) {
        OwnershipGuard ownership_guard(response_ownership, response_owner_id);

        if (ownership_guard.have_ownership()) {
            response->begin(false);

            if (rc != TF_E_OK) {
                response->writef("Failed to get energy manager daily data point: error %d", rc);
                logger.printfln("energy_manager: Failed to get energy manager daily data point: error %d", rc);
            }
            else if (status != 0) {
                response->writef("Failed to get energy manager daily data point: status %u", status);
                logger.printfln("energy_manager: Failed to get energy manager daily data point: status %u", status);
            }

            response->flush();
            response->end("");
        }
    }
    else {
        StreamMetadata *metadata = &metadata_array[3];

        metadata->response = response;
        metadata->response_ownership = response_ownership;
        metadata->response_owner_id = response_owner_id;
        metadata->call_begin = true;
        metadata->write_comma = false;
        metadata->next_offset = 0;

        tf_warp_energy_manager_register_sd_energy_manager_daily_data_points_low_level_callback(&device, energy_manager_daily_data_points_handler, metadata);
    }

    check_bricklet_reachable(rc, "history_energy_manager_daily_response");
}
