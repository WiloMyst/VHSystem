"""
@file nlp_server.py
@brief 独立声学映射微服务 (Acoustic Mapping Microservice)
@details 
在最新的异构计算架构中，大模型 (LLM) 的流式推理与动态断句已下沉至 C++ 核心引擎 (llama.cpp) 处理。
本模块被彻底精简为纯粹的 NLP 计算节点，专职负责将 C++ 下发的纯文本语义块 (Semantic Chunks) 
映射为底层 TTS 模型所需的音素 ID 张量序列 (Phoneme IDs)。
通过极简的 TCP 持久化连接作为 IPC (跨进程通信) 桥梁，确保 CPU 密集型的分词注音任务与主渲染循环物理隔离。
"""

import socket
import json
import piper_phonemize  # 原生 Piper 语音合成引擎的注音基建模块


def load_config(config_path: str) -> dict:
    """
    @brief 加载 TTS 模型的声学映射字典与特殊 Token 规范
    @param config_path 配置文件物理路径
    @return 反序列化后的字典对象
    """
    with open(config_path, 'r', encoding='utf-8') as f:
        return json.load(f)


def text_to_ids(text: str, config: dict) -> list:
    """
    @brief 核心映射管线：将自然语言纯文本转化为一维张量序列
    @param text 剥离了标点符号的最小语义语句 (由 C++ 引擎切割并下发)
    @param config 包含特殊 Token (BOS, EOS, PAD) 映射规则的配置集
    @return 序列化后的音素整数数组 (Phoneme IDs)
    """
    id_map = config['phoneme_id_map']
    
    # 提取序列首尾边界与填充描述符，配置 Fallback 容错机制防崩溃
    bos_id = config.get('bos') or id_map.get('^', [1])[0]
    eos_id = config.get('eos') or id_map.get('$', [2])[0]
    pad_id = config.get('pad') or id_map.get('_', [0])[0]

    # 调用 C++ 扩展底层的 espeak-ng 引擎执行拼音/音调解构 (当前适配普通话 cmn)
    phonemes_list = piper_phonemize.phonemize_espeak(text, "cmn")
    phoneme_ids = [bos_id]
    
    # 展平多级嵌套数组并高频注入静音对齐标识 (PAD)，以符合 Piper 模型的输入契约
    for phonemes in phonemes_list:
        for p in phonemes:
            if p in id_map:
                phoneme_ids.extend(id_map[p])
                phoneme_ids.append(pad_id)
                
    phoneme_ids.append(eos_id)
    return phoneme_ids


def run_server():
    """
    @brief 启动微服务主事件循环 (Main Event Loop)
    """
    # 预加载全局映射字典，避免在 RPC 热路径 (Hot Path) 中发生 I/O 阻塞
    config = load_config("models/zh_CN-huayan-medium.onnx.json")
    
    # 初始化跨进程 TCP 通信套接字
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # 开启 SO_REUSEADDR 端口复用，保障微服务在崩溃重启时能无缝抢占原端口
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    # 绑定本地环回地址，谢绝外部网络直接访问，保障内部 IPC 链路安全
    server.bind(('127.0.0.1', 50052))
    server.listen(50) 
    
    print("[NLP Server] 声学映射计算节点已启动，正在监听 IPC 端口 127.0.0.1:50052...")

    while True:
        # 阻塞等待 C++ 核心引擎的主动连接
        conn, addr = server.accept()
        print(f"[NLP Server] [+] 建立底层持久化链路 (Keep-Alive): {addr}")
        
        try:
            # 采用 file-like object 包装 socket，基于 \n 定界符彻底规避 TCP 粘包解析难题
            with conn, conn.makefile('rw', encoding='utf-8') as f:
                while True: 
                    # 阻塞读取 C++ 引擎的同步 RPC 调用请求
                    data = f.readline() 
                    if not data:
                        print(f"[NLP Server] [-] 探测到远端 TCP FIN 信号，C++ 核心已主动断开: {addr}")
                        break 
                    
                    text_chunk = data.strip()
                    if not text_chunk:
                        continue
                    
                    # print(f"  [RPC 接收] 提取待注音语义块: {text_chunk}")
                    
                    # =========================================================
                    # [核心] 执行 CPU 密集型注音映射
                    # =========================================================
                    ids = text_to_ids(text_chunk, config)
                    
                    # 序列化为 JSON 数组，并严格附加换行符 \n 以契合 C++ 端的 readline 契约
                    f.write(json.dumps(ids) + '\n')
                    f.flush() # 强制冲刷网卡缓冲区，压缩跨进程通信延迟 (IPC Latency)
                    
        except Exception as e:
            print(f"[NLP Server] [!] 运行期上下文发生严重异常: {e}")

if __name__ == '__main__':
    run_server()