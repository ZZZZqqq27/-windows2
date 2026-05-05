# 两级多级测试说明

## 测试类型

**Chunk 级并发下载测试**：测试两台机器跨网络环境下，不同节点规模与不同并发水平的内容获取性能。

## 设计说明

- **固定入口设计**：Mac侧使用 `A0` 节点作为客户端固定入口，WSL侧使用 `B0` 节点作为客户端固定入口
- **数据流向**：文件先上传到 Mac 侧 `A0` 节点，通过跨机 DHT 网络传播后，两侧分别下载

## 文件说明

| 文件名 | 说明 | 在哪台机器跑 |
|--------|------|------------|
| `start_machineA.sh` | 启动Mac节点（A0, A1, ...） | Mac侧 |
| `start_machineB.sh` | 启动WSL节点（B0, B1, ...） | WSL侧 |
| `prepare_dataset.sh` | 准备数据集+上传到A0 | Mac侧 |
| `download_mac.sh` | Mac侧通过A0下载前24个chunk | Mac侧 |
| `download_wsl.sh` | WSL侧通过B0下载后24个chunk | WSL侧 |

## 前置准备

**1. 修改脚本里的 IP 地址**

在所有脚本开头，修改：
```bash
MAC_IP="192.168.1.100"  # 换成你的Mac IP
WSL_IP="192.168.1.101" # 换成你的WSL IP
```

**2. 编译程序**

确保 `/Users/zhangzhiqing/Desktop/-windows/pro/build/app` 已编译成功。

---

## 测试流程（3次跑完，每次跑一组节点配置）

---

### 第1次：跑 N4 组（2+2节点）

| 机器 | 步骤 | 命令 |
|------|------|------|
| **Mac** | 1. 启动Mac节点 | `cd /path/to/两级多级测试; chmod +x *.sh; ./start_machineA.sh 2` |
| **WSL** | 2. 启动WSL节点 | `cd /path/to/两级多级测试; chmod +x *.sh; ./start_machineB.sh 2` |
| **Mac** | 3. 准备数据集 | `./prepare_dataset.sh` |
| **Mac** | 4. 复制文件到WSL | 复制 `output/dual_host/env_*.sh` 和 `两级多级测试/chunk_ids_*.txt` 到WSL（**两个文件都要复制到WSL的脚本目录**） |
| **Mac** | 5. Mac侧下载 | `./download_mac.sh /path/to/env_*.sh 2 2` |
| **WSL** | 6. WSL侧下载 | `./download_wsl.sh /path/to/env_*.sh 2 2` |
| **WSL** | 7. 复制结果到Mac | 复制WSL侧的 `runs_wsl_*.tsv` 和 `group_summary_wsl_*.tsv` 到Mac |
| **两台** | 8. 清理进程 | `pkill -9 -f "build/app"` |

---

### 第2次：跑 N8 组（4+4节点）

| 机器 | 步骤 | 命令 |
|------|------|------|
| **Mac** | 1. 启动Mac节点 | `./start_machineA.sh 4` |
| **WSL** | 2. 启动WSL节点 | `./start_machineB.sh 4` |
| **Mac** | 3. 准备数据集 | `./prepare_dataset.sh` |
| **Mac** | 4. 复制文件到WSL | 复制新的 env_*.sh 和 chunk_ids_*.txt 到WSL |
| **Mac** | 5. Mac侧下载 | `./download_mac.sh /path/to/new/env_*.sh 4 4` |
| **WSL** | 6. WSL侧下载 | `./download_wsl.sh /path/to/new/env_*.sh 4 4` |
| **WSL** | 7. 复制结果到Mac | 复制WSV侧TSV结果到Mac |
| **两台** | 8. 清理进程 | `pkill -9 -f "build/app"` |

---

### 第3次：跑 N16 组（8+8节点）

| 机器 | 步骤 | 命令 |
|------|------|------|
| **Mac** | 1. 启动Mac节点 | `./start_machineA.sh 8` |
| **WSL** | 2. 启动WSL节点 | `./start_machineB.sh 8` |
| **Mac** | 3. 准备数据集 | `./prepare_dataset.sh` |
| **Mac** | 4. 复制文件到WSL | 复制新生成的文件到WSL |
| **Mac** | 5. Mac侧下载 | `./download_mac.sh /path/to/env_*.sh 8 8` |
| **WSL** | 6. WSL侧下载 | `./download_wsl.sh /path/to/env_*.sh 8 8` |
| **WSL** | 7. 复制结果到Mac | 复制结果到Mac |
| **两台** | 8. 清理进程 | `pkill -9 -f "build/app"` |

---

## 测试参数说明

| 参数 | 值 | 说明 |
|------|-----|------|
| 文件大小 | 0.1MB (100KB) | 快速测试，避免太慢 |
| 文件总数 | 48个 | Mac侧24，WSL侧24 |
| 每轮测试 | 2次 | 减少时间 |
| 并发度 | 4, 8, 16 | 三个维度，均为真实并发 |
| 节点分布 | 2+2, 4+4, 8+8 | 三组节点规模：N4、N8、N16 |

---

## 输出位置

数据统一输出到 **Mac侧**，方便查看：

| 文件 | 路径 |
|------|------|
| Mac侧逐轮 | `/Users/zhangzhiqing/Desktop/-windows/output/dual_host/summary/runs_mac_*.tsv` |
| Mac侧汇总 | `/Users/zhangzhiqing/Desktop/-windows/output/dual_host/summary/group_summary_mac_*.tsv` |
| WSL侧逐轮 | 复制到Mac同目录，命名为 runs_wsl_*.tsv |
| WSL侧汇总 | 复制到Mac同目录，命名为 group_summary_wsl_*.tsv |
| chunk_id列表 | `/Users/zhangzhiqing/Desktop/-windows/两级多级测试/chunk_ids_*.txt` |
| 环境变量 | `/Users/zhangzhiqing/Desktop/-windows/output/dual_host/env_*.sh` |

---

## 跟 Kubo 对比建议

建议只用 **N4（2+2节点）** 做简单对比：
- 你的系统：正常跑上面的流程
- Kubo：用 `ipfs add` 添加同批文件，然后 `ipfs cat` 测试下载吞吐量

对比维度：单文件下载吞吐量。

---

## 清理残留

跑完每轮后清理进程：
```bash
pkill -9 -f "build/app"
```

清理测试数据（可选）：
```bash
rm -rf /Users/zhangzhiqing/Desktop/-windows/output/dual_host
rm -rf /Users/zhangzhiqing/Desktop/-windows/data/dual_host
```
