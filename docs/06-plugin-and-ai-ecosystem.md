# 06. 统一资源中心、开放插件与 AI 生态 (Plugin & AI Ecosystem)

## 1. 架构目标：告别私有资产黑盒

传统剪辑软件往往采用私有的预设打包格式，导致生态割裂且资产迁移成本极高。

OliveLan 确立了 **Phase 17「统一资源中心（Universal Resource Hub）」** 的核心指导思想：**零私有格式架构**。

```text
外部素材目录 / 音效包 / GL-Transitions 着色器
                      │
                      ▼
        本地目录只读挂载 (Folder Mount)
                      │
        ┌─────────────┴─────────────┐
        ▼                           ▼
常驻停靠资源面板 (Dock Panel)    独立大窗资源管理 (Manager Window)
        │                           │
        └─────────────┬─────────────┘
                      ▼
           异步元数据提取与秒级预览
                      │
                      ▼
         拖拽直通时间线 (Zero-Copy Link)
```

---

## 2. 核心架构设计 (Phase 17)

### 2.1 本地目录只读挂载 (`Folder Mount`)
- 用户可直接将本地文件夹（如音效库、BGM 目录、常用视频片段、本地剪映缓存 `Cache/effect`）作为外部资源库挂载；
- 系统建立基于 SQLite 的轻量只读索引，按需提取时长、分辨率、通道与波形缩略图，**不拷贝、不破坏源文件**。

### 2.2 开放转场与片元着色器直通
- 接入行业标准的 **[GL-Transitions](https://gl-transitions.com/)** 规范；
- 允许用户直接放置纯 `.glsl` 转场代码，无需编写 C++ 插件，直接由内部渲染管线（`OpenGLRenderGraph`）将其动态编译为转场着色器，参数通过 JSON 自动生成界面控件。

### 2.3 进程隔离型开放插件沙箱 (Stdio JSON-RPC / MCP)
为了彻底避免第三方 C++ 动态链接库因指针越界导致整机崩溃（Crash）：
- **微内核沙箱**：外部命令与扩展以独立的子进程运行（支持 Python、Node.js、Rust 等）；
- **协议标准化**：宿主与插件通过标准输入输出（Stdio）以 JSON-RPC 2.0 / MCP 数据包通信；
- **原子事务保护**：所有插件对工程的修改统一封装为 `MultiUndoCommand` 事务，保证 Ctrl+Z 可完整一键回退。

---

## 3. 基于 MCP 的 AI 剪辑集成 (Phase 16)

系统通过内部嵌入的标准 **Model Context Protocol (MCP)** 服务，打通大语言模型与端云多模态 AI 算力：

1. **Olive MCP Server**：
   - 将时间线操作抽象为标准的 Tools（如 `split_clip`、`add_transition`、`search_resources`）；
   - 将工程上下文抽象为 Resources（如 `timeline://active/tracks`、`footage://metadata`）；
2. **智能化剪辑流水线**：
   - **智能切除静音段 (Silence Cut)**：结合 Whisper 语音模型对齐音频时间戳，秒级剔除无效停顿；
   - **自动初剪 (Smart Assembly)**：AI 根据提纲与分镜文本，调用 Phase 17 资源中心检索合适素材并粗剪入轨；
   - **人像抠像 (Alpha Pass-through)**：接收外部推理引擎生成的单通道 R8 蒙版纹理，显存原位与调色层混合。
