// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.analysis;

import com.starrocks.catalog.PrimitiveType;
import com.starrocks.catalog.ScalarType;
import com.starrocks.catalog.Type;
import com.starrocks.common.AnalysisException;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

import java.util.TimeZone;

public class LiteralExprCompareTest {

    @BeforeAll
    public static void setUp() {
        TimeZone tz = TimeZone.getTimeZone("ETC/GMT-0");
        TimeZone.setDefault(tz);
    }

    @Test
    public void boolTest() {
        LiteralExpr boolTrue1 = new BoolLiteral(true);
        LiteralExpr boolFalse1 = new BoolLiteral(false);
        LiteralExpr boolTrue2 = new BoolLiteral(true);

        // value equal
        Assertions.assertTrue(boolTrue1.equals(boolTrue2));
        // self equal
        Assertions.assertTrue(boolTrue1.equals(boolTrue1));

        // value compare
        Assertions.assertTrue(!boolTrue1.equals(boolFalse1) && 1 == boolTrue1.compareLiteral(boolFalse1));
        Assertions.assertTrue(-1 == boolFalse1.compareLiteral(boolTrue1));
        // value equal
        Assertions.assertTrue(0 == boolTrue1.compareLiteral(boolTrue2));
        // self equal
        Assertions.assertTrue(0 == boolTrue1.compareLiteral(boolTrue1));
    }

    @Test
    public void dateFormat1Test() throws AnalysisException {
        LiteralExpr date = new DateLiteral("2015-02-15 12:12:12", ScalarType.DATE);
    }

    @Test
    public void dateFormat2Test() throws AnalysisException {
        LiteralExpr datetime = new DateLiteral("2015-02-15", ScalarType.DATETIME);
    }

    @Test
    public void dateTest() throws AnalysisException {
        LiteralExpr date1 = new DateLiteral("2015-02-15", ScalarType.DATE);
        LiteralExpr date1Same = new DateLiteral("2015-02-15", ScalarType.DATE);
        LiteralExpr date1Large = new DateLiteral("2015-02-16", ScalarType.DATE);
        LiteralExpr datetime1 = new DateLiteral("2015-02-15 13:14:00", ScalarType.DATETIME);
        LiteralExpr datetime1Same = new DateLiteral("2015-02-15 13:14:00", ScalarType.DATETIME);
        LiteralExpr datetime1Large = new DateLiteral("2015-02-15 13:14:15", ScalarType.DATETIME);

        // infinity
        LiteralExpr maxDate1 = new DateLiteral(ScalarType.DATE, true);
        LiteralExpr maxDate1Same = new DateLiteral(ScalarType.DATE, true);
        LiteralExpr minDate1 = new DateLiteral(ScalarType.DATE, false);
        LiteralExpr minDate1Same = new DateLiteral(ScalarType.DATE, false);
        LiteralExpr maxDatetime1 = new DateLiteral(ScalarType.DATETIME, true);
        LiteralExpr maxDatetime1Same = new DateLiteral(ScalarType.DATETIME, true);
        LiteralExpr minDatetime1 = new DateLiteral(ScalarType.DATETIME, false);
        LiteralExpr minDatetime1Same = new DateLiteral(ScalarType.DATETIME, false);
        LiteralExpr date8 = new DateLiteral("9999-12-31", ScalarType.DATE);
        LiteralExpr date9 = new DateLiteral("9999-12-31 23:59:59.999999", ScalarType.DATETIME);
        LiteralExpr date10 = new DateLiteral("0000-01-01", ScalarType.DATE);
        LiteralExpr date11 = new DateLiteral("0000-01-01 00:00:00", ScalarType.DATETIME);

        Assertions.assertTrue(date1.equals(date1Same) && date1.compareLiteral(date1Same) == 0);
        Assertions.assertTrue(date1.equals(date1Same) && date1.compareLiteral(date1Same) == 0);
        Assertions.assertTrue(datetime1.equals(datetime1Same) && datetime1.compareLiteral(datetime1Same) == 0);
        Assertions.assertTrue(datetime1.equals(datetime1) && datetime1.compareLiteral(datetime1) == 0);

        // value compare
        Assertions.assertTrue(!date1Large.equals(date1Same) && 1 == date1Large.compareLiteral(date1Same));
        Assertions.assertTrue(!datetime1Large.equals(datetime1Same) && 1 == datetime1Large.compareLiteral(datetime1Same));
        Assertions.assertTrue(!datetime1Same.equals(datetime1Large) && -1 == datetime1Same.compareLiteral(datetime1Large));

        // infinity
        Assertions.assertTrue(maxDate1.equals(maxDate1) && maxDate1.compareLiteral(maxDate1) == 0);
        Assertions.assertTrue(maxDate1.equals(maxDate1Same) && maxDate1.compareLiteral(maxDate1Same) == 0);
        Assertions.assertTrue(minDate1.equals(minDate1) && minDate1.compareLiteral(minDate1) == 0);
        Assertions.assertTrue(minDate1.equals(minDate1Same) && minDate1.compareLiteral(minDate1Same) == 0);
        Assertions.assertTrue(maxDatetime1.equals(maxDatetime1) && maxDatetime1.compareLiteral(maxDatetime1) == 0);
        Assertions.assertTrue(maxDatetime1.equals(maxDatetime1Same) && maxDatetime1.compareLiteral(maxDatetime1Same) == 0);
        Assertions.assertTrue(minDatetime1.equals(minDatetime1) && minDatetime1.compareLiteral(minDatetime1) == 0);
        Assertions.assertTrue(minDatetime1.equals(minDatetime1Same) && minDatetime1.compareLiteral(minDatetime1Same) == 0);

        Assertions.assertTrue(maxDate1.equals(date8) && maxDate1.compareLiteral(date8) == 0);
        Assertions.assertTrue(minDate1.equals(date10) && minDate1.compareLiteral(date10) == 0);
        Assertions.assertTrue(maxDatetime1.equals(date9) && maxDatetime1.compareLiteral(date9) == 0);
        Assertions.assertTrue(minDatetime1.equals(date11) && minDatetime1.compareLiteral(date11) == 0);

        Assertions.assertTrue(!maxDate1.equals(date1) && 1 == maxDate1.compareLiteral(date1));
        Assertions.assertTrue(!minDate1.equals(date1) && -1 == minDate1.compareLiteral(date1));
        Assertions.assertTrue(!maxDatetime1.equals(datetime1) && 1 == maxDatetime1.compareLiteral(datetime1));
        Assertions.assertTrue(!minDatetime1.equals(datetime1) && -1 == minDatetime1.compareLiteral(datetime1));
    }

    @Test
    public void decimalTest() throws AnalysisException {
        LiteralExpr decimal1 = new DecimalLiteral("1.23456");
        LiteralExpr decimal2 = new DecimalLiteral("1.23456");
        LiteralExpr decimal3 = new DecimalLiteral("1.23457");
        LiteralExpr decimal4 = new DecimalLiteral("2.23457");

        // value equal
        Assertions.assertTrue(decimal1.equals(decimal2));
        // self equal
        Assertions.assertTrue(decimal1.equals(decimal1));

        // value compare
        Assertions.assertTrue(!decimal3.equals(decimal2) && 1 == decimal3.compareLiteral(decimal2));
        Assertions.assertTrue(!decimal4.equals(decimal3) && 1 == decimal4.compareLiteral(decimal3));
        Assertions.assertTrue(!decimal1.equals(decimal4) && -1 == decimal1.compareLiteral(decimal4));
        // value equal
        Assertions.assertTrue(0 == decimal1.compareLiteral(decimal2));
        // self equal
        Assertions.assertTrue(0 == decimal1.compareLiteral(decimal1));
    }

    @Test
    public void floatAndDoubleExpr() throws AnalysisException {
        LiteralExpr float1 = new FloatLiteral(1.12345, ScalarType.FLOAT);
        LiteralExpr float2 = new FloatLiteral(1.12345, ScalarType.FLOAT);
        LiteralExpr float3 = new FloatLiteral(1.12346, ScalarType.FLOAT);
        LiteralExpr float4 = new FloatLiteral(2.12345, ScalarType.FLOAT);

        LiteralExpr double1 = new FloatLiteral(1.12345, ScalarType.DOUBLE);
        LiteralExpr double2 = new FloatLiteral(1.12345, ScalarType.DOUBLE);
        LiteralExpr double3 = new FloatLiteral(1.12346, ScalarType.DOUBLE);
        LiteralExpr double4 = new FloatLiteral(2.12345, ScalarType.DOUBLE);

        // float
        // value equal
        Assertions.assertTrue(float1.equals(float2));
        // self equal
        Assertions.assertTrue(float1.equals(float1));

        // value compare
        Assertions.assertTrue(!float3.equals(float2) && 1 == float3.compareLiteral(float2));
        Assertions.assertTrue(!float4.equals(float1) && 1 == float4.compareLiteral(float1));
        Assertions.assertTrue(!float1.equals(float4) && -1 == float1.compareLiteral(float4));
        // value equal
        Assertions.assertTrue(0 == float1.compareLiteral(float2));
        // self equal
        Assertions.assertTrue(0 == float1.compareLiteral(float1));

        // double
        // value equal
        Assertions.assertTrue(double1.equals(double2));
        // self equal
        Assertions.assertTrue(double1.equals(double1));

        // value compare
        Assertions.assertTrue(!double3.equals(double2) && 1 == double3.compareLiteral(double2));
        Assertions.assertTrue(!double4.equals(double1) && 1 == double4.compareLiteral(double1));
        Assertions.assertTrue(!double1.equals(double4) && -1 == double1.compareLiteral(double4));
        // value equal
        Assertions.assertTrue(0 == double1.compareLiteral(double2));
        // self equal
        Assertions.assertTrue(0 == double1.compareLiteral(double1));

        LiteralExpr floatType = LiteralExpr.create("3.14", Type.FLOAT);
        Assertions.assertEquals(PrimitiveType.FLOAT, floatType.getType().getPrimitiveType());
        Assertions.assertEquals(true, floatType.equals(new FloatLiteral(3.14, Type.FLOAT)));

        LiteralExpr doubleType = LiteralExpr.create("3.14", Type.DOUBLE);
        Assertions.assertEquals(PrimitiveType.DOUBLE, doubleType.getType().getPrimitiveType());
        Assertions.assertEquals(true, doubleType.equals(new FloatLiteral(3.14, Type.DOUBLE)));
    }

    private void intTestInternal(ScalarType type) throws AnalysisException {
        String maxValue = "";
        String minValue = "";
        String normalValue = "100";

        switch (type.getPrimitiveType()) {
            case TINYINT:
                maxValue = "127";
                minValue = "-128";
                break;
            case SMALLINT:
                maxValue = "32767";
                minValue = "-32768";
                break;
            case INT:
                maxValue = "2147483647";
                minValue = "-2147483648";
                break;
            case BIGINT:
                maxValue = "9223372036854775807";
                minValue = "-9223372036854775808";
                break;
            default:
                Assertions.fail();
        }

        LiteralExpr tinyint1 = new IntLiteral(maxValue, type);
        LiteralExpr tinyint2 = new IntLiteral(maxValue, type);
        LiteralExpr tinyint3 = new IntLiteral(minValue, type);
        LiteralExpr tinyint4 = new IntLiteral(normalValue, type);

        // infinity
        LiteralExpr infinity1 = MaxLiteral.MAX_VALUE;
        LiteralExpr infinity2 = MaxLiteral.MAX_VALUE;
        LiteralExpr infinity3 = LiteralExpr.createInfinity(type, false);
        LiteralExpr infinity4 = LiteralExpr.createInfinity(type, false);

        // value equal
        Assertions.assertTrue(tinyint1.equals(tinyint1));
        // self equal
        Assertions.assertTrue(tinyint1.equals(tinyint2));

        // value compare
        Assertions.assertTrue(!tinyint1.equals(tinyint3) && 1 == tinyint1.compareLiteral(tinyint3));
        Assertions.assertTrue(!tinyint2.equals(tinyint4) && 1 == tinyint2.compareLiteral(tinyint4));
        Assertions.assertTrue(!tinyint3.equals(tinyint4) && -1 == tinyint3.compareLiteral(tinyint4));
        // value equal
        Assertions.assertTrue(0 == tinyint1.compareLiteral(tinyint1));
        // self equal
        Assertions.assertTrue(0 == tinyint1.compareLiteral(tinyint2));

        // infinity
        Assertions.assertTrue(infinity1.equals(infinity1));
        Assertions.assertTrue(infinity1.equals(infinity2));
        Assertions.assertTrue(infinity3.equals(infinity3));
        Assertions.assertTrue(infinity3.equals(infinity4));
        Assertions.assertFalse(tinyint1.equals(infinity1));
        Assertions.assertTrue(tinyint3.equals(infinity3));

        Assertions.assertTrue(0 == infinity1.compareLiteral(infinity1));
        Assertions.assertTrue(0 == infinity1.compareLiteral(infinity2));
        Assertions.assertTrue(!infinity1.equals(infinity3) && 1 == infinity1.compareLiteral(infinity3));
        Assertions.assertTrue(!infinity4.equals(infinity2) && -1 == infinity4.compareLiteral(infinity2));

        Assertions.assertTrue(!infinity4.equals(tinyint1) && -1 == infinity4.compareLiteral(tinyint1));
        Assertions.assertTrue(!infinity3.equals(tinyint4) && -1 == infinity3.compareLiteral(tinyint4));

        Assertions.assertTrue(infinity1.compareLiteral(tinyint2) == 1);
        Assertions.assertTrue(0 == infinity4.compareLiteral(tinyint3));
    }

    @Test
    public void intTest() throws AnalysisException {
        intTestInternal(ScalarType.createType(PrimitiveType.TINYINT));
        intTestInternal(ScalarType.createType(PrimitiveType.SMALLINT));
        intTestInternal(ScalarType.createType(PrimitiveType.INT));
        intTestInternal(ScalarType.createType(PrimitiveType.BIGINT));
    }

    @Test
    public void largeIntTest() throws AnalysisException {
        LiteralExpr largeInt1 = new LargeIntLiteral("170141183460469231731687303715884105727");
        LiteralExpr largeInt3 = new LargeIntLiteral("-170141183460469231731687303715884105728");

        LiteralExpr infinity1 = new LargeIntLiteral(true);
        LiteralExpr infinity3 = new LargeIntLiteral(false);

        // value equal
        Assertions.assertTrue(largeInt1.equals(largeInt1));

        // value compare
        Assertions.assertTrue(!largeInt1.equals(largeInt3) && 1 == largeInt1.compareLiteral(largeInt3));
        // value equal
        Assertions.assertTrue(0 == largeInt1.compareLiteral(largeInt1));

        // infinity
        Assertions.assertTrue(infinity1.equals(infinity1));
        Assertions.assertTrue(infinity3.equals(infinity3));
        Assertions.assertTrue(infinity1.equals(largeInt1));
        Assertions.assertTrue(infinity3.equals(largeInt3));

        Assertions.assertTrue(!infinity1.equals(largeInt3) && 1 == infinity1.compareLiteral(largeInt3));
        Assertions.assertTrue(!infinity3.equals(infinity1) && -1 == infinity3.compareLiteral(infinity1));

        Assertions.assertTrue(0 == infinity1.compareLiteral(infinity1));
        Assertions.assertTrue(0 == infinity3.compareLiteral(infinity3));
        Assertions.assertTrue(0 == infinity1.compareLiteral(largeInt1));
        Assertions.assertTrue(0 == infinity3.compareLiteral(largeInt3));
    }

    @Test
    public void stringTest() throws AnalysisException {
        LiteralExpr string1 = new StringLiteral("abc");
        LiteralExpr string2 = new StringLiteral("abc");
        LiteralExpr string3 = new StringLiteral("bcd");
        LiteralExpr string4 = new StringLiteral("a");
        LiteralExpr string5 = new StringLiteral("aa");
        LiteralExpr empty = new StringLiteral("");

        Assertions.assertTrue(string1.equals(string1) && string1.compareLiteral(string2) == 0);
        Assertions.assertTrue(string1.equals(string2) && string1.compareLiteral(string1) == 0);

        Assertions.assertTrue(!string3.equals(string1) && 1 == string3.compareLiteral(string1));
        Assertions.assertTrue(!string1.equals(string3) && -1 == string1.compareLiteral(string3));
        Assertions.assertTrue(!string5.equals(string4) && 1 == string5.compareLiteral(string4));
        Assertions.assertTrue(!string3.equals(string4) && 1 == string3.compareLiteral(string4));
        Assertions.assertTrue(!string4.equals(empty) && 1 == string4.compareLiteral(empty));
    }

}
