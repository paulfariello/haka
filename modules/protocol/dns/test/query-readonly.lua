-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

local dns = require("protocol/dns")

dns.install_udp_rule(53)

haka.rule {
	hook = dns.events.response,
	eval = function (dns, response, query)
		query.id = 0x2811
	end
}
