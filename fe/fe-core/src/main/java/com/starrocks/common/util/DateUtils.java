// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.starrocks.common.util;

import com.starrocks.common.AnalysisException;

import java.time.LocalDate;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.format.DateTimeFormatter;
import java.time.format.DateTimeFormatterBuilder;
import java.time.format.SignStyle;
import java.time.temporal.ChronoField;
import java.time.temporal.TemporalAccessor;

public class DateUtils {
    // These are marked as deprecated because they don't support year 0000 parsing
    @Deprecated
    public static final DateTimeFormatter DATE_FORMATTER = DateTimeFormatter.ofPattern("yyyy-MM-dd");
    @Deprecated
    public static final DateTimeFormatter DATEKEY_FORMATTER = DateTimeFormatter.ofPattern("yyyyMMdd");
    @Deprecated
    public static final DateTimeFormatter DATE_TIME_FORMATTER = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss");
    @Deprecated
    public static final DateTimeFormatter MINUTE_FORMATTER = DateTimeFormatter.ofPattern("yyyyMMddHHmm");
    @Deprecated
    public static final DateTimeFormatter HOUR_FORMATTER = DateTimeFormatter.ofPattern("yyyyMMddHH");
    @Deprecated
    public static final DateTimeFormatter YEAR_FORMATTER = DateTimeFormatter.ofPattern("yyyy");
    @Deprecated
    public static final DateTimeFormatter QUARTER_FORMATTER = DateTimeFormatter.ofPattern("yyyy'Q'q");
    @Deprecated
    public static final DateTimeFormatter MONTH_FORMATTER = DateTimeFormatter.ofPattern("yyyyMM");

    public static final DateTimeFormatter TIME_FORMATTER = DateTimeFormatter.ofPattern("HH:mm:ss");

    public static final DateTimeFormatter DATE_FORMATTER_UNIX =
            DateUtils.unixDatetimeFormatBuilder("%Y-%m-%d").toFormatter();
    public static final DateTimeFormatter DATEKEY_FORMATTER_UNIX =
            DateUtils.unixDatetimeFormatBuilder("%Y%m%d").toFormatter();
    public static final DateTimeFormatter DATE_TIME_FORMATTER_UNIX =
            DateUtils.unixDatetimeFormatBuilder("%Y-%m-%d %H:%i:%s").toFormatter();
    public static final DateTimeFormatter DATE_TIME_MS_FORMATTER_UNIX =
            DateUtils.unixDatetimeFormatBuilder("%Y-%m-%d %H:%i:%s.%f").toFormatter();
    public static final DateTimeFormatter MINUTE_FORMATTER_UNIX =
            DateUtils.unixDatetimeFormatBuilder("%Y%m%d%H%i").toFormatter();
    public static final DateTimeFormatter HOUR_FORMATTER_UNIX =
            DateUtils.unixDatetimeFormatBuilder("%Y%m%d%H").toFormatter();
    public static final DateTimeFormatter YEAR_FORMATTER_UNIX =
            DateUtils.unixDatetimeFormatBuilder("%Y").toFormatter();
    public static final DateTimeFormatter MONTH_FORMATTER_UNIX =
            DateUtils.unixDatetimeFormatBuilder("%Y%m").toFormatter();
    public static DateTimeFormatter probeFormat(String dateTimeStr) throws AnalysisException {
        if (dateTimeStr.length() == 8) {
            return DateUtils.DATEKEY_FORMATTER;
        } else if (dateTimeStr.length() == 10) {
            return DateUtils.DATE_FORMATTER_UNIX;
        } else if (dateTimeStr.length() == 19) {
            return DateUtils.DATE_TIME_FORMATTER_UNIX;
        } else {
            throw new AnalysisException("can not probe datetime format:" + dateTimeStr);
        }
    }

    /*
     * Parse datetime string use formatter, and if hour/minute/second is null will fill 00:00:00
     * */
    public static LocalDateTime parseStringWithDefaultHSM(String datetime, DateTimeFormatter formatter) {
        TemporalAccessor temporal = formatter.parse(datetime);
        if (temporal.isSupported(ChronoField.HOUR_OF_DAY) && temporal.isSupported(ChronoField.SECOND_OF_MINUTE) &&
                temporal.isSupported(ChronoField.MINUTE_OF_HOUR)) {
            return LocalDateTime.from(temporal);
        } else {
            return LocalDateTime.of(LocalDate.from(temporal), LocalTime.of(0, 0, 0));
        }
    }

    public static DateTimeFormatterBuilder unixDatetimeFormatBuilder(String pattern) {
        return unixDatetimeFormatBuilder(pattern, true);
    }

    public static DateTimeFormatterBuilder unixDatetimeFormatBuilder(String pattern, boolean isOutputFormat) {
        DateTimeFormatterBuilder builder = new DateTimeFormatterBuilder();
        boolean escaped = false;
        for (int i = 0; i < pattern.length(); i++) {
            char character = pattern.charAt(i);
            if (escaped) {
                switch (character) {
                    case 'c': // %c Month, numeric (0..12)
                        builder.appendValue(ChronoField.MONTH_OF_YEAR, 1, 2, SignStyle.NORMAL);
                        break;
                    case 'm': // %m Month, numeric (00..12)
                        if (isOutputFormat) {
                            builder.appendValue(ChronoField.MONTH_OF_YEAR, 2);
                        } else {
                            builder.appendValue(ChronoField.MONTH_OF_YEAR, 1, 2, SignStyle.NORMAL);
                        }
                        break;
                    case 'd': // %d Day of the month, numeric (00..31)
                        if (isOutputFormat) {
                            builder.appendValue(ChronoField.DAY_OF_MONTH, 2);
                        } else {
                            builder.appendValue(ChronoField.DAY_OF_MONTH, 1, 2, SignStyle.NORMAL);
                        }
                        break;
                    case 'e': // %e Day of the month, numeric (0..31)
                        builder.appendValue(ChronoField.DAY_OF_MONTH, 1, 2, SignStyle.NORMAL);
                        break;
                    case 'H': // %H Hour (00..23)
                        builder.appendValue(ChronoField.HOUR_OF_DAY, 2);
                        break;
                    case 'k': // %k Hour (0..23)
                        builder.appendValue(ChronoField.HOUR_OF_DAY, 1, 2, SignStyle.NORMAL);
                        break;
                    case 'h': // %h Hour (01..12)
                    case 'I': // %I Hour (01..12)
                        builder.appendValue(ChronoField.CLOCK_HOUR_OF_AMPM, 2);
                        break;
                    case 'l': // %l Hour (1..12)
                        builder.appendValue(ChronoField.CLOCK_HOUR_OF_AMPM, 1, 2, SignStyle.NORMAL);
                        break;
                    case 'i': // %i Minutes, numeric (00..59)
                        builder.appendValue(ChronoField.MINUTE_OF_HOUR, 2);
                        break;
                    case 'j': // %j Day of year (001..366)
                        builder.appendValue(ChronoField.DAY_OF_YEAR, 3);
                        break;
                    case 'S': // %S Seconds (00..59)
                    case 's': // %s Seconds (00..59)
                        builder.appendValue(ChronoField.SECOND_OF_MINUTE, 2);
                        break;
                    case 'T': // %T Time, 24-hour (hh:mm:ss)
                        builder.appendValue(ChronoField.HOUR_OF_DAY, 2)
                                .appendLiteral(':')
                                .appendValue(ChronoField.MINUTE_OF_HOUR, 2)
                                .appendLiteral(':')
                                .appendValue(ChronoField.SECOND_OF_MINUTE, 2);
                        break;
                    case 'v': // %v Week (01..53), where Monday is the first day of the week; used with %x
                        builder.appendValue(ChronoField.ALIGNED_WEEK_OF_YEAR, 2);
                        break;
                    case 'Y': // %Y Year, numeric, four digits
                        builder.appendValue(ChronoField.YEAR, 4);
                        break;
                    case 'y': // %y Year, numeric (two digits)
                        builder.appendValueReduced(ChronoField.YEAR_OF_ERA, 2, 2, 1970)
                                .parseDefaulting(ChronoField.ERA, 1);
                        break;
                    case 'f': // %f Microseconds (000000..999999)
                        if (isOutputFormat) {
                            builder.padNext(6, '0');
                        }
                        builder.appendFraction(ChronoField.MICRO_OF_SECOND, 1, 6, false);
                        break;
                    case 'u': // %u Week (00..53), where Monday is the first day of the week
                        builder.appendValueReduced(ChronoField.ALIGNED_WEEK_OF_YEAR, 2, 2, 0);
                        break;
                    case 'r': // %r Time, 12-hour (hh:mm:ss followed by AM or PM), Java can't convert language
                    case 'p': // %p AM or PM, Java can't convert language
                    case 'w': // %w Day of the week (0=Sunday..6=Saturday), Java only support 1~7
                    case 'U': // %U Week (00..53), where Sunday is the first day of the week
                    case 'W': // %W Weekday name (Sunday..Saturday)
                    case 'x': // %x Year for the week, where Monday is the first day of the week, numeric, four digits; used with %v
                    case 'M': // %M Month name (January..December)
                    case 'a': // %a Abbreviated weekday name (Sun..Sat)
                    case 'b': // %b Abbreviated month name (Jan..Dec)
                    case 'V': // %V Week (01..53), where Sunday is the first day of the week; used with %X
                    case 'X': // %X Year for the week where Sunday is the first day of the week, numeric, four digits; used with %V
                    case 'D': // %D Day of the month with English suffix (0th, 1st, 2nd, 3rd, ...)
                        throw new IllegalArgumentException(
                                String.format("%%%s not supported in date format string", character));
                    case '%': // %% A literal "%" character
                        builder.appendLiteral('%');
                        break;
                    default: // %<x> The literal character represented by <x>
                        builder.appendLiteral(character);
                        break;
                }
                escaped = false;
            } else if (character == '%') {
                escaped = true;
            } else {
                builder.appendLiteral(character);
            }
        }
        return builder;
    }
}