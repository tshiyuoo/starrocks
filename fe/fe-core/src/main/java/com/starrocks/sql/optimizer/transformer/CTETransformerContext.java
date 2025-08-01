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


package com.starrocks.sql.optimizer.transformer;

import com.google.common.base.Preconditions;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

public class CTETransformerContext {
    private final Map<Integer, ExpressionMapping> cteExpressions;

    // cteMould -> current cte ref
    private final Map<Integer, Integer> cteRefIdMapping;

    // Records the total number of OptExpression nodes for each CTE Producer.
    // When the node count of cte is 0, disable the force reuse optimization.
    // cte id -> node count
    private final Map<Integer, Integer> cteIdToNodeCount;
    
    private final AtomicInteger uniqueId;

    private final int cteMaxLimit;

    public CTETransformerContext(int cteMaxLimit) {
        this.cteExpressions = new HashMap<>();
        this.cteRefIdMapping = new HashMap<>();
        this.cteIdToNodeCount = new HashMap<>();
        this.uniqueId = new AtomicInteger();
        this.cteMaxLimit = cteMaxLimit;
    }

    public Map<Integer, ExpressionMapping> getCteExpressions() {
        return cteExpressions;
    }

    /*
     * Regenerate cteId, we do inline CTE in transform phase, needs
     * to be promised that the cteId generated each time is different
     *
     * e.g.
     *   with x1 (
     *       with x2 (select * from t0) select * from x2
     *   )
     *   select * from x1;
     *
     * Transform result without inline cte:
     *
     *                             CTEAnchor1
     *                          /             \
     *                    CTEProduce1         CTEConsume1
     *                       /
     *                CTEAnchor2
     *                 /       \
     *         CTEProduce2    CTEConsume2
     *               /
     *          Scan(t0)
     *
     * But we do inline cte in transform phase now, the result will be:
     *                             CTEAnchor1
     *                          /             \
     *                    CTEProduce1         CTEConsume1
     *                       /                        \
     *                CTEAnchor2                    CTEAnchor2-1
     *                 /       \                     /       \
     *         CTEProduce2    CTEConsume2    CTEProduce2-1    CTEConsume2-1
     *               /             \               /             \
     *          Scan(t0)        Scan(t0)      Scan(t0)           Scan(t0)
     *
     *  CTEAnchor2 and CTEAnchor2-1 are from same CTE (with x2), but have different cteID
     *  So, generate the cteID everytime on one CTE instance.
     */
    public int registerCte(int cteMouldId) {
        cteRefIdMapping.put(cteMouldId, uniqueId.incrementAndGet());
        return cteRefIdMapping.get(cteMouldId);
    }

    public void recordCteNodeCount(int cteId, int nodeCount) {
        cteIdToNodeCount.put(cteId, nodeCount);
    }

    public Integer getCteNodeCount(int cteId) {
        return cteIdToNodeCount.get(cteId);
    }

    public boolean hasRegisteredCte(int cteMouldId) {
        return cteRefIdMapping.containsKey(cteMouldId);
    }

    public int getCurrentCteRef(int cteMouldId) {
        Preconditions.checkState(cteRefIdMapping.containsKey(cteMouldId));
        return cteRefIdMapping.get(cteMouldId);
    }

    public boolean isForceInline() {
        return cteRefIdMapping.size() > cteMaxLimit;
    }
}
