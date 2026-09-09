/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
/*
 * tz_table.c - curated world timezone table.
 *
 * Each entry maps a friendly alias and an IANA-style place name to the
 * POSIX TZ string newlib actually consumes - including the DST transition
 * rules, which are exactly the part nobody should hand-type. Deliberately
 * curated rather than exhaustive: zones with unstable or exotic rules
 * (e.g. Iran, Chatham) are omitted; the raw-POSIX path in `tz` remains for
 * those. Rules current as of 2026; a table refresh is a one-file edit.
 */
#include "tz_table.h"

const whm_tz_entry_t k_tz_table[] = {
    /* --- UTC --- */
    { "utc",          "Etc/UTC",                "UTC0" },
    /* --- Americas --- */
    { "pacific",      "America/Los_Angeles",    "PST8PDT,M3.2.0,M11.1.0" },
    { "mountain",     "America/Denver",         "MST7MDT,M3.2.0,M11.1.0" },
    { "arizona",      "America/Phoenix",        "MST7" },
    { "central",      "America/Chicago",        "CST6CDT,M3.2.0,M11.1.0" },
    { "eastern",      "America/New_York",       "EST5EDT,M3.2.0,M11.1.0" },
    { "alaska",       "America/Anchorage",      "AKST9AKDT,M3.2.0,M11.1.0" },
    { "hawaii",       "Pacific/Honolulu",       "HST10" },
    { "atlantic",     "America/Halifax",        "AST4ADT,M3.2.0,M11.1.0" },
    { "newfoundland", "America/St_Johns",       "NST3:30NDT,M3.2.0,M11.1.0" },
    { "mexico-city",  "America/Mexico_City",    "CST6" },
    { "bogota",       "America/Bogota",         "COT5" },
    { "lima",         "America/Lima",           "PET5" },
    { "caracas",      "America/Caracas",        "VET4" },
    { "sao-paulo",    "America/Sao_Paulo",      "BRT3" },
    { "buenos-aires", "America/Argentina",      "ART3" },
    /* --- Europe --- */
    { "london",       "Europe/London",          "GMT0BST,M3.5.0/1,M10.5.0" },
    { "dublin",       "Europe/Dublin",          "GMT0IST,M3.5.0/1,M10.5.0" },
    { "lisbon",       "Europe/Lisbon",          "WET0WEST,M3.5.0/1,M10.5.0" },
    { "paris",        "Europe/Paris",           "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "berlin",       "Europe/Berlin",          "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "madrid",       "Europe/Madrid",          "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "rome",         "Europe/Rome",            "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "athens",       "Europe/Athens",          "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "helsinki",     "Europe/Helsinki",        "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "kyiv",         "Europe/Kyiv",            "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "istanbul",     "Europe/Istanbul",        "TRT-3" },
    { "moscow",       "Europe/Moscow",          "MSK-3" },
    /* --- Africa / Middle East --- */
    { "cairo",        "Africa/Cairo",           "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    { "lagos",        "Africa/Lagos",           "WAT-1" },
    { "johannesburg", "Africa/Johannesburg",    "SAST-2" },
    { "nairobi",      "Africa/Nairobi",         "EAT-3" },
    { "jerusalem",    "Asia/Jerusalem",         "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "dubai",        "Asia/Dubai",             "GST-4" },
    /* --- Asia --- */
    { "karachi",      "Asia/Karachi",           "PKT-5" },
    { "india",        "Asia/Kolkata",           "IST-5:30" },
    { "dhaka",        "Asia/Dhaka",             "BDT-6" },
    { "bangkok",      "Asia/Bangkok",           "ICT-7" },
    { "jakarta",      "Asia/Jakarta",           "WIB-7" },
    { "singapore",    "Asia/Singapore",         "SGT-8" },
    { "hongkong",     "Asia/Hong_Kong",         "HKT-8" },
    { "china",        "Asia/Shanghai",          "CST-8" },
    { "taipei",       "Asia/Taipei",            "CST-8" },
    { "tokyo",        "Asia/Tokyo",             "JST-9" },
    { "seoul",        "Asia/Seoul",             "KST-9" },
    /* --- Oceania --- */
    { "perth",        "Australia/Perth",        "AWST-8" },
    { "adelaide",     "Australia/Adelaide",     "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "brisbane",     "Australia/Brisbane",     "AEST-10" },
    { "sydney",       "Australia/Sydney",       "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "melbourne",    "Australia/Melbourne",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "auckland",     "Pacific/Auckland",       "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "fiji",         "Pacific/Fiji",           "FJT-12" },
};

const int k_tz_count = sizeof(k_tz_table) / sizeof(k_tz_table[0]);
