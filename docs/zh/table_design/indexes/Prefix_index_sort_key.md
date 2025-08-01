---
displayed_sidebar: docs
keywords: ['suoyin']
sidebar_position: 10
---

# 前缀索引和排序键

## 功能简介

建表时指定一个或多个列构成排序键 (Sort Key)。表中的数据行会根据排序键进行排序以后再落入磁盘存储。

**并且数据写入的过程中会自动生成前缀索引。数据按照指定的排序键排序后，每写入 1024 行数据构成一个逻辑数据块（Data Block），在前缀索引表中存储一个索引项，内容为该逻辑数据块中第一行数据的排序列组成的前缀。**

通过这样两层的排序结构，查询时就可以使用二分查找快速跳过不符合条件的数据。

:::tip

前缀索引是一种稀疏索引，其大小至少会比数据量小 1024 倍，因此一般可以全量缓存在内存中，加速查询性能。

:::

## 使用说明

自 3.0 版本起，主键表支持使用 `ORDER BY` 定义排序键，自 3.3 版本起，明细表、聚合表和更新表支持使用 `ORDER BY` 定义排序键。

- 明细表中数据按照排序键 `ORDER BY` 排序，排序键可以为任意列的排列组合。
  :::info
  如果同时指定 `ORDER BY` 和 `DUPLICATE KEY`，则 `DUPLICATE KEY` 不生效。
  :::
- 聚合表中数据先按照聚合键 `AGGREGATE KEY` 进行聚合后，再按照排序键 `ORDER BY` 排序。`ORDER BY` 和 `AGGREGATE KEY` 中的列需要保持一致，但是列的顺序不需要保持一致。
- 更新表中数据先按照唯一键 `UNIQUE KEY` 进行 REPLACE 后，再按照排序键 `ORDER BY` 排序。`ORDER BY` 和 `UNIQUE KEY` 中的列需要保持一致，但是列的顺序不需要保持一致。
- 主键表中数据先按照主键 `PRIMARY KEY` 进行 REPLACE 后，再按照排序键 `ORDER BY` 排序。

以明细表为例，使用 `ORDER BY` 定义排序键为 `uid` 和 `name`。

```SQL
CREATE TABLE user_access (
    uid int,
    name varchar(64),
    age int, 
    phone varchar(16),
    last_access datetime,
    credits double
)
ORDER BY (uid, name);
```

:::tip

建表后可以通过 `SHOW CREATE TABLE <table_name>;` 在返回结果中的 `ORDER BY` 子句中查看指定的排序列和排序列的顺序。

:::

由于前缀索引项的最大长度为 36 字节，超过部分会被截断，因此该表的前缀索引项为 uid (4 字节) + name (只取前 32 字节)，前缀字段为 `uid` 和 `name`。

**注意事项**

- 前缀字段的数量不超过 3 个，前缀索引项的最大长度为 36 字节。

- 前缀字段中 CHAR、VARCHAR、STRING 类型的列只能出现一次，并且处在末尾位置。

  假设存在下表，其中前三列为排序列，前缀字段为 `name`（20 字节）。即使索引项没有达到 36 个字节，因为前缀索引以 VARCHAR 类型的列开始，所以直接截断，不再往后继续。所以，这里前缀索引只含有字段 `name`。

    ```SQL
    MySQL [example_db]> describe user_access2;
    +-------------+-------------+------+-------+---------+-------+
    | Field       | Type        | Null | Key   | Default | Extra |
    +-------------+-------------+------+-------+---------+-------+
    | name        | varchar(20) | YES  | true  | NULL    |       |
    | uid         | int         | YES  | true  | NULL    |       |
    | last_access | datetime    | YES  | true  | NULL    |       |
    | age         | int         | YES  | false | NULL    |       |
    | phone       | varchar(16) | YES  | false | NULL    |       |
    | credits     | double      | YES  | false | NULL    |       |
    +-------------+-------------+------+-------+---------+-------+
    6 rows in set (0.00 sec)
    ```

- 如果表中通过 `ORDER BY` 指定了排序键，就根据排序键构建前缀索引；如果没有通过 `ORDER BY` 指定排序键，就根据 Key 列构建前缀索引。

### 如何设计合理排序键，以便查询利用前缀索引加速

根据分析业务场景中查询和数据特点，选择合理的排序列和设计排序列的顺序，来组成前缀索引，能够显著提高查询性能。

- 排序列不宜过多， 一般为 3 个，建议不超过 4 个。排序列过多并不有助于提升查询性能，反而会导致数据导入时会增加排序的开销。
- 选择排序列和排序列的顺序，从以下两个方面按优先级展开：
  
  1. **选择经常作为查询过滤条件的列为排序列**。如果存在多个排序列，则按照作为查询条件列的频率排列，最经常作为查询过滤条件的排序列放最前面。
  
      这样查询的过滤条件包含**前缀索引的前缀**，则查询性能可以得到显著提升。并且如果过滤条件中包含索引的全部前缀，则查询可以充分借助前缀索引，从而获得最佳提升效果。当然，过滤条件中未包含全部前缀，但是只要包含前缀，前缀索引也能优化查询。不过过滤条件包含的前缀长度太短，则会减弱索引的效果。
      
      例如仍然以上[明细表](#使用说明) `user_access` 为例进行说明，该表的前缀字段为 `uid` 和 `name`。如果查询过滤条件包含全部的前缀，比如 `select sum(credits) from user_access where uid = 123 and name = 'Jane Smith';`， 则查询可以充分利用前缀索引来提升性能。
      
      如果查询条件只包含部分前缀 ，比如 `select sum(credits) from user_access where uid = 123;`，查询也可以适当借助前缀索引用于提升查询性能。
      
      然而，如果查询条件不包含前缀，例如`select sum(credits) from user_access where name = 'Jane Smith';`，则查询无法借助前缀索引加速。
  
  2. 如果多个排序列作为查询过滤条件的频率差不多，则可以衡量各排序列的基数特点。
     - 列的基数较高，则查询时能够过滤较多的数据。如果列的基数过低，比如布尔类型的列，则查询时其对于数据过滤效果不佳。
       
       :::tip
       
       然而考虑到实际业务场景中的查询特点，通常相比于高基数列，基于基数稍低的列进行查询过滤会更频繁一些。因为如果经常基于高基数列过滤的话，甚至在一些极端的场景中，经常基于具有唯一性约束的列进行查询过滤的话，则这类查询实际上会偏向于 OLTP 数据库中点查了，而不是 OLAP 数据库中的复杂分析性查询了。
       
       :::
     - 另外考虑存储压缩因素。如果一个低基数列和一个高基数列谁前谁后对于查询性能影响不大，则将低基数列在高基数列前时，排序后的低基数列的存储压缩率会高很多，因此建议低基数列放前。
     
### 建表时定义排序列的注意事项

- 排序列的数据类型：
  - 主键表的排序列支持数值（包括整型、布尔）、字符串、时间日期类型。
  - 明细表、聚合表和更新表的排序列支持数值（包括整型、布尔、Decimal）、字符串、时间日期类型。

- 聚合表和更新表中，排序列必须定义在其他列之前。

### 是否支持修改前缀索引

如果业务场景中查询特点发生变化，查询条件经常使用前缀字段之外的列，现有的前缀索引无法过滤数据，此时查询性能可能不佳。

自 3.0 版本起，支持修改主键表的排序键，自 3.3 版本起，支持修改明细表、聚合表和更新表的排序键。明细表和主键表中排序键可以为任意列的排序组合，聚合表和更新表中排序键必须包含所有 key 列，但是列的顺序无需与 key 列保持一致。

或者，您还可以基于该表创建[同步物化视图](../../using_starrocks/Materialized_view-single_table.md)，并基于其它常用的条件过滤列构建前缀索引，从而提升查询性能。但是注意这样会增加存储空间。

## 如何判断前缀索引是否生效

执行查询后，您可以通过 [Query Profile](../../best_practices/query_tuning/query_profile_overview.md) 的 scan 节点中的详细指标查看前缀索引是否生效以及过滤效果，例如 `ShortKeyFilterRows` 等指标。
