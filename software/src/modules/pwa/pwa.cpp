/* esp32-firmware
 * Copyright (C) 2023 Frederic Henrichs <frederic@tinkerforge.com>
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

#include "pwa.h"
#include "web_server.h"

// add this to index.html.template
    // <link rel="apple-touch-icon" type="image/png" href="data:image/png;base64,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"/>
    // <link rel="manifest" href="manifest.json"/>

void Pwa::pre_setup() {}

void Pwa::setup() {
    initialized = true;
}

void Pwa::register_urls() {
    server.on("/manifest.json", HTTP_GET, [](WebServerRequest request) {
        return request.send(200, "application/json", "{\"theme_color\": \"#5d5d5d\",\"background_color\": \"#5d5d5d\",\"display\": \"standalone\",\"scope\": \"/\",\"icons\": [{\"src\": \"data:image/png;base64,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\",\"sizes\": \"192x192\",\"type\": \"image/png\",\"purpose\": \"maskable any\"},{\"src\": \"data:image/png;base64,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\",\"sizes\": \"512x512\",\"type\": \"image/png\",\"purpose\": \"maskable any\"}], \"prefer_related_applications\": true, \"start_url\": \"/#status\", \"name\": \"Warp Charger\", \"short_name\": \"Warp\"}");
    });
}

void Pwa::loop() {}
