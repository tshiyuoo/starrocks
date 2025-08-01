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

package com.starrocks.sql.optimizer.operator.scalar;

import com.google.common.base.Joiner;
import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.Lists;
import com.starrocks.catalog.StructField;
import com.starrocks.catalog.StructType;
import com.starrocks.catalog.Type;
import com.starrocks.sql.optimizer.base.ColumnRefSet;
import com.starrocks.sql.optimizer.operator.OperatorType;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

public class SubfieldOperator extends ArgsScalarOperator {
    // Only one child
    private final ImmutableList<String> fieldNames;
    private boolean copyFlag = true;

    // Build based on SlotRef which contains struct subfield access information
    public static SubfieldOperator build(ScalarOperator child, Type type, List<Integer> usedSubfieldPos) {
        Type tmpType = type;
        // Like SELECT a.b.c FROM tbl; Will be converted to:
        // Subfield(ColumnRefOperator(a), ["b", "c"])
        List<String> usedSubfieldNames = new ArrayList<>();
        for (int pos : usedSubfieldPos) {
            StructType structType = (StructType) tmpType;
            StructField field = structType.getField(pos);
            usedSubfieldNames.add(field.getName());
            tmpType = field.getType();
        }
        return new SubfieldOperator(child, tmpType, ImmutableList.copyOf(usedSubfieldNames));
    }

    public SubfieldOperator(ScalarOperator child, Type type, List<String> fieldNames) {
        this(child, type, fieldNames, true);
    }

    public SubfieldOperator(ScalarOperator child, Type type, List<String> fieldNames, boolean copyFlag) {
        super(OperatorType.SUBFIELD, type);
        this.arguments.add(child);
        this.fieldNames = ImmutableList.copyOf(fieldNames);
        this.copyFlag = copyFlag;
        incrDepth(arguments);
    }

    public List<String> getFieldNames() {
        return fieldNames;
    }

    public boolean getCopyFlag() {
        return copyFlag;
    }

    public void setCopyFlag(boolean copyFlag) {
        this.copyFlag = copyFlag;
    }


    @Override
    public boolean isNullable() {
        return arguments.get(0).isNullable();
    }

    @Override
    public ScalarOperator getChild(int index) {
        Preconditions.checkArgument(index == 0);
        return arguments.get(0);
    }

    @Override
    public void setChild(int index, ScalarOperator child) {
        Preconditions.checkArgument(index == 0);
        arguments.set(0, child);
    }

    @Override
    public ScalarOperator clone() {
        SubfieldOperator subfieldOperator = (SubfieldOperator) super.clone();
        // Deep copy here
        List<ScalarOperator> newChildren = Lists.newArrayList();
        this.arguments.forEach(p -> newChildren.add(p.clone()));
        subfieldOperator.arguments = newChildren;
        return subfieldOperator;
    }

    @Override
    public String toString() {
        return String.format("Subfield([%s], \"%s\")", getChild(0).toString(), Joiner.on('.').join(fieldNames));
    }

    @Override
    public int hashCodeSelf() {
        return Objects.hash(opType, fieldNames, copyFlag);
    }

    @Override
    public boolean equalsSelf(Object other) {
        if (other == this) {
            return true;
        }

        if (!(other instanceof SubfieldOperator otherOp)) {
            return false;
        }
        return fieldNames.equals(otherOp.fieldNames) && getChild(0).equals(otherOp.getChild(0))
                && copyFlag == otherOp.getCopyFlag();
    }

    @Override
    public <R, C> R accept(ScalarOperatorVisitor<R, C> visitor, C context) {
        return visitor.visitSubfield(this, context);
    }

    @Override
    public ColumnRefSet getUsedColumns() {
        return getChild(0).getUsedColumns();
    }

    public String getPath() {
        String childPath = getChildPath();
        return childPath + "." + Joiner.on('.').join(fieldNames);
    }

    private String getChildPath() {
        if (arguments.get(0) instanceof ColumnRefOperator) {
            return ((ColumnRefOperator) arguments.get(0)).getName();
        }
        return arguments.get(0).toString();
    }
}
