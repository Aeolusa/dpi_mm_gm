# CHI BFM DPI-C Golden Memory 架构说明

## 1. 系统总览

```mermaid
graph TD
    subgraph SV["SystemVerilog 层"]
        TOP["mm_gm_top\n（顶层模块）"]
        SM0["mm_gm_bfm\nMST_IDX=0\n(SM0)"]
        SM1["mm_gm_bfm\nMST_IDX=1\n(SM1)"]
        SMN["mm_gm_bfm\nMST_IDX=N-1\n(SMn)"]
        HST["mm_gm_bfm\nMST_IDX=SM_NUMS\n(HOST)"]
        TS["mm_gm_bfm\nMST_IDX=SM_NUMS+1\n(TS)"]
        BLIT["mm_gm_bfm\nMST_IDX=SM_NUMS+2\n(BLIT)"]
        TOP --> SM0 & SM1 & SMN & HST & TS & BLIT
    end

    subgraph DPI["DPI-C 接口层 (mm_gm_dpi.cc)"]
        F1["dpi_chi_txreq\n(mst_idx, txnid, addr, size, opcode, secvec, time)"]
        F2["dpi_chi_rxrsp_dbid\n(mst_idx, txnid, opcode, dbid)"]
        F3["dpi_chi_txdat\n(mst_idx, txnid=dbid!, opcode, dataid, data[255:0], be, data_cnt)"]
        F4["dpi_chi_rxdat\n(mst_idx, txnid, opcode, dataid, data[255:0], be, data_cnt)"]
    end

    subgraph CPP["C++ 处理层"]
        MGR["ChiTransactionManager"]
        WR_MAP["outstanding_writes_\nMap&lt;MstKey, ChiOutstandingWrite&gt;"]
        RD_MAP["outstanding_reads_\nMap&lt;MstKey, ChiOutstandingRead&gt;"]
        DBID_MAP["dbid_to_txnid_\nMap&lt;MstKey{mst,dbid}, txnid&gt;"]
        CHK["ConsistencyChecker"]
        SMEM["ShadowMemory\n(稀疏页表, 128B Block)"]
    end

    SV -->|"4个DPI函数"| DPI
    DPI --> MGR
    MGR --> WR_MAP & RD_MAP & DBID_MAP
    MGR --> CHK --> SMEM
```

---

## 2. MST_IDX 编号规则

| BFM实例 | MST_IDX | 说明 |
|---------|---------|------|
| SM0 | 0 | genvar idx=0 |
| SM1 | 1 | genvar idx=1 |
| ... | ... | ... |
| SM(N-1) | SM_NUMS-1 | genvar idx=N-1 |
| HOST | SM_NUMS | 固定=8（默认SM_NUMS=8） |
| TS | SM_NUMS+1 | 固定=9 |
| BLIT | SM_NUMS+2 | 固定=10 |

> **关键**：同一个BFM实例内，txnid空间相互独立。不同BFM可能同时使用相同txnid，通过 `MstKey = {mst_idx, txnid}` 在C++侧唯一区分。

---

## 3. 写事务（WrNoSnp）三阶段流程

```mermaid
sequenceDiagram
    participant SV as SV BFM<br/>(MST_IDX=m)
    participant DPI as mm_gm_dpi.cc
    participant MGR as ChiTransactionManager
    participant MAP as outstanding_writes_<br/>dbid_to_txnid_
    participant CHK as ConsistencyChecker
    participant MEM as ShadowMemory

    Note over SV,MEM: 阶段1：txreq 建立写请求

    SV->>DPI: dpi_chi_txreq(m, txnid=T, addr, size,<br/>opcode=WrNoSnp, secvec=S, time)
    DPI->>MGR: process_txreq(m, T, addr, size, opcode, S, time)
    MGR->>MAP: 写入 outstanding_writes_[{m,T}]<br/>expected_flits = popcount(S)

    Note over SV,MEM: 阶段2：rxrsp 收到 DBID

    SV->>DPI: dpi_chi_rxrsp_dbid(m, txnid=T,<br/>opcode=DBIDResp, dbid=D)
    DPI->>MGR: process_rxrsp_dbid(m, T, opcode, D)
    MGR->>MAP: outstanding_writes_[{m,T}].dbid = D<br/>dbid_to_txnid_[{m,D}] = T

    Note over SV,MEM: 阶段3：txdat 传输数据（1~4笔）

    loop 每笔 NCBWrData flit (共 expected_flits 笔)
        SV->>DPI: dpi_chi_txdat(m, txnid=D, opcode,<br/>dataid=ID, data[255:0], be, data_cnt)
        Note right of SV: ⚠️ NCBWrData.txnid = DBID!
        DPI->>MGR: process_txdat(m, D, opcode, ID,<br/>flit_bytes[32], be_mask, data_cnt)
        MGR->>MAP: 查 dbid_to_txnid_[{m,D}] → T<br/>data_buf[ID*32 : ID*32+31] = flit_bytes<br/>be_buf[...] |= be_mask<br/>accumulated_flits++
        
        alt SM类型 且 data_cnt>0 (首笔)
            MGR->>MAP: expected_flits = data_cnt
        end
        
        alt accumulated_flits >= expected_flits
            MGR->>CHK: process_write(Transaction)
            CHK->>MEM: write(addr, data[128B], byte_enable)
            MGR->>MAP: 清除 outstanding_writes_[{m,T}]<br/>清除 dbid_to_txnid_[{m,D}]
        end
    end
```

---

## 4. 读事务（RdNoSnp）两阶段流程

```mermaid
sequenceDiagram
    participant SV as SV BFM<br/>(MST_IDX=m)
    participant DPI as mm_gm_dpi.cc
    participant MGR as ChiTransactionManager
    participant MAP as outstanding_reads_
    participant CHK as ConsistencyChecker
    participant MEM as ShadowMemory

    Note over SV,MEM: 阶段1：txreq 建立读请求

    SV->>DPI: dpi_chi_txreq(m, txnid=T, addr, size,<br/>opcode=RdNoSnp, secvec=S, time)
    DPI->>MGR: process_txreq(m, T, addr, size, opcode, S, time)
    MGR->>MAP: 写入 outstanding_reads_[{m,T}]<br/>expected_flits = popcount(S)

    Note over SV,MEM: 阶段2：rxdat 收到 CompData（1~4笔）

    loop 每笔 CompData flit (共 expected_flits 笔)
        SV->>DPI: dpi_chi_rxdat(m, txnid=T,<br/>opcode=CompData, dataid=ID,<br/>data[255:0], be, data_cnt)
        Note right of SV: rxdat.txnid = txreq.txnid ✓
        DPI->>MGR: process_rxdat(m, T, opcode, ID,<br/>flit_bytes[32], be_mask, data_cnt)
        MGR->>MAP: data_buf[ID*32 : ID*32+31] = flit_bytes<br/>be_buf[...] |= be_mask<br/>accumulated_flits++
        
        alt accumulated_flits >= expected_flits
            MGR->>CHK: process_read(Transaction)
            CHK->>MEM: read_byte(addr+i) per valid byte
            CHK-->>MGR: CheckReport (PASS / FAIL)
            MGR->>MAP: 清除 outstanding_reads_[{m,T}]
        end
    end
```

---

## 5. secvec 与 dataid 数据拼接

### 5.1 secvec：有效段选择（4-bit）

```
                 1024-bit Cacheline (128 Bytes)
┌────────────┬────────────┬────────────┬────────────┐
│  Bytes     │  Bytes     │  Bytes     │  Bytes     │
│  [0 : 31]  │  [32: 63]  │  [64: 95]  │  [96:127]  │
│  (256-bit) │  (256-bit) │  (256-bit) │  (256-bit) │
└────────────┴────────────┴────────────┴────────────┘
      ↑              ↑           ↑            ↑
   secvec[0]    secvec[1]   secvec[2]    secvec[3]

示例：secvec = 4'b0101 (= 0x5)
  → bit0=1: bytes[0:31]   有效 ✓
  → bit1=0: bytes[32:63]  无效 ✗
  → bit2=1: bytes[64:95]  有效 ✓
  → bit3=0: bytes[96:127] 无效 ✗

expected_flits = popcount(0x5) = 2
```

### 5.2 dataid：flit在缓冲区中的位置

```
dataid[1:0] │ byte_offset = dataid * 32
────────────┼──────────────────────────
   2'b00    │  0  → data_buf[0  :31 ]
   2'b01    │  32 → data_buf[32 :63 ]
   2'b10    │  64 → data_buf[64 :95 ]
   2'b11    │  96 → data_buf[96 :127]
```

### 5.3 完整数据组装示例（secvec=0xA，写事务）

```
secvec=4'b1010：bit1=1(bytes[32:63]), bit3=1(bytes[96:127])
expected_flits = 2

flit①: dataid=2'b01 → data_buf[32:63] = flit_data, be_buf[32:63] = be_mask
flit②: dataid=2'b11 → data_buf[96:127]= flit_data, be_buf[96:127]= be_mask

最终128B缓冲：
┌────────────┬────────────┬────────────┬────────────┐
│ xxxxxxxxxx │ ▓▓▓▓▓flit①│ xxxxxxxxxx │ ▓▓▓▓▓flit②│
│ be=false   │ be=per-bit │ be=false   │ be=per-bit │
└────────────┴────────────┴────────────┴────────────┘
                         ↓
              ShadowMemory.write()
              只更新 byte_enable=true 的字节
```

---

## 6. data_cnt 特性（SM 专属）

```mermaid
flowchart TD
    A["收到第一笔 txdat flit\n(来自SM接口)"] --> B{data_cnt > 0?}
    B -- 是 SM 类型 --> C["expected_flits = data_cnt\nuse_data_cnt = true"]
    B -- 否/其他类型 --> D["expected_flits 保持\npopcount(secvec) 初始值"]
    C --> E["按 dataid 存入缓冲"]
    D --> E
    E --> F{"accumulated_flits\n>= expected_flits?"}
    F -- No --> G["等待下一笔 txdat"]
    G --> E
    F -- Yes --> H["finalize_write()\n→ ConsistencyChecker"]
```

> **注意**：`data_cnt` 只在 SM 接口有意义。其他接口（HOST/TS/BLIT）传 `data_cnt=0`，以 `secvec` 的 popcount 为准。

---

## 7. 一致性检查机制

```mermaid
flowchart LR
    subgraph WRITE["写路径"]
        W1["finalize_write()"] --> W2["ConsistencyChecker\n::process_write()"] --> W3["ShadowMemory::write()\n按 byte_enable 更新\n128B 块内对应字节"]
    end

    subgraph READ["读路径"]
        R1["finalize_read()"] --> R2["ConsistencyChecker\n::process_read()"]
        R2 --> R3{对128B中\n每个byte_enable=true的字节}
        R3 --> R4{"曾经被写过?"}
        R4 -- No --> R5["strict: FAIL_NO_PRIOR_WRITE\n宽松: 跳过"]
        R4 -- Yes --> R6{"overlap\n时间窗口内?"}
        R6 -- Yes --> R7["check 值在可能值集合内\n(宽松序模型)"]
        R6 -- No --> R8["精确比对\nactual == expected?"]
        R8 -- Fail --> R9["FAIL_DATA_MISMATCH\n打印写历史"]
        R7 -- Fail --> R9
        R8 -- Pass --> R10["PASS"]
        R7 -- Pass --> R10
    end
```

---

## 8. 修改文件一览

| 文件 | 修改内容 |
|------|---------|
| `include/types.h` | 新增 CHI 常量（`CHI_FLIT_BYTES`=32, `CHI_CL_BYTES`=128）、opcode枚举、`secvec_to_flits()`、`dataid_to_offset()`；`CACHELINE_SIZE`→128 |
| `include/transaction.h` | 添加 `secvec` 字段；`src_id` 语义→`mst_idx`；`byte_en_at()` 默认改为 `false`；更新 `to_string()` |
| `include/chi_transaction_manager.h` | **全面重构**：`TxnKey`→`MstKey{mst_idx,txn_id}`；新增 `ChiOutstandingRead`；更新所有方法签名 |
| `src/chi_transaction_manager.cc` | **全面重构**：实现读写分离、dataid组装、data_cnt支持、dbid逆向映射 |
| `src/mm_gm_dpi.cc` | **全面重构**：新增 `mst_idx`；`dpi_chi_txreq_write`→`dpi_chi_txreq`；`unpack_flit256()` |
| `src/consistency_check.cc` | `process_read()` 新增 `byte_en_at(i)` 跳过逻辑；`track_active_write()` 步长改为 `CHI_CL_BYTES` |
| `src/transaction_manager.cc` | 修复 `txn.master_id`→`txn.src_id`；新增 `tgt_id`/`dbid`/`secvec` 初始化 |
| `test/unit_test_checker.cc` | `make_write_txn`/`make_read_txn` 新增 `secvec=0xF` 初始化 |
| `sv/mm_gm_bfm.sv` | 新增 `MST_IDX` 参数；所有DPI函数加 `mst_idx` 参数；`dpi_chi_txreq_write`→`dpi_chi_txreq`；读写txreq合并 |
| `sv/mm_gm_top.sv` | 所有BFM实例化添加 `.MST_IDX(idx/SM_NUMS/SM_NUMS+1/SM_NUMS+2)` |
