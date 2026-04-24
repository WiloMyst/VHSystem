import socket
import json
import re
import piper_phonemize # 注音
from openai import OpenAI

# =====================================================================
# 1. 初始化大模型客户端 (接入本地 Ollama 算力引擎)
# =====================================================================
llm_client = OpenAI(
    api_key="ollama-local",                    # 本地部署不需要真实的 Key，随便填一个即可通过 SDK 校验
    base_url="http://127.0.0.1:11434/v1"       # 指向 Ollama 的本地服务端口
)

# 匹配中文和英文的断句标点符号
PUNCTUATION_PATTERN = re.compile(r'([。！？；.!?;\n])')

def load_config(config_path):
    with open(config_path, 'r', encoding='utf-8') as f:
        return json.load(f)

def text_to_ids(text, config):
    id_map = config['phoneme_id_map']
    bos_id = config.get('bos') or id_map.get('^', [1])[0]
    eos_id = config.get('eos') or id_map.get('$', [2])[0]
    pad_id = config.get('pad') or id_map.get('_', [0])[0]

    phonemes_list = piper_phonemize.phonemize_espeak(text, "cmn")
    phoneme_ids = [bos_id]
    
    for phonemes in phonemes_list:
        for p in phonemes:
            if p in id_map:
                phoneme_ids.extend(id_map[p])
                phoneme_ids.append(pad_id)
                
    phoneme_ids.append(eos_id)
    return phoneme_ids

def run_server():
    config = load_config("models/zh_CN-huayan-medium.onnx.json")
    
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', 50052))
    server.listen(50) 
    
    print("[LLM Server] 思考中枢已启动，监听 127.0.0.1:50052...")

    while True:
        conn, addr = server.accept()
        print(f"[LLM Server] [+] 建立持久长连接: {addr}")
        
        try:
            with conn, conn.makefile('rw', encoding='utf-8') as f:
                while True: 
                    data = f.readline() 
                    if not data:
                        print(f"[LLM Server] [-] C++ 核心已主动断开连接: {addr}")
                        break 
                    
                    user_prompt = data.strip()
                    if not user_prompt:
                        continue
                    
                    print(f"\n[玩家提问] ❯ {user_prompt}")
                    print("[LLM 思考中] 流式生成与下发...")
                    
                    # =========================================================
                    # 2. 触发大模型流式推理 (Streaming)
                    # =========================================================
                    response = llm_client.chat.completions.create(
                        model="qwen2.5:1.5b", # 本地跑的 Qwen 模型
                        messages=[
                            {"role": "system", "content": "你是一个亲切的老奶奶虚拟人。回答要简短、口语化，每次回答最多两三句话，不要用复杂的词汇。"},
                            {"role": "user", "content": user_prompt}
                        ],
                        stream=True,  
                        temperature=0.7
                    )
                    
                    buffer_text = ""
                    
                    # 3. 拦截数据流，进行动态断句
                    for chunk in response:
                        if chunk.choices[0].delta.content:
                            char = chunk.choices[0].delta.content
                            buffer_text += char
                            
                            # 遇到标点符号，立刻斩断并发送给 C++
                            if PUNCTUATION_PATTERN.search(char):
                                sentence = buffer_text.strip()
                                if sentence:
                                    print(f"  [切片下发] -> {sentence}")
                                    ids = text_to_ids(sentence, config)
                                    f.write(json.dumps(ids) + '\n')
                                    f.flush()
                                buffer_text = "" # 清空缓冲，装下一句话
                                
                    # 处理结尾可能没有标点符号的残留文字
                    if buffer_text.strip():
                        print(f"  [切片下发] -> {buffer_text.strip()}")
                        ids = text_to_ids(buffer_text.strip(), config)
                        f.write(json.dumps(ids) + '\n')
                        f.flush()
                        
                    # 4. 关键动作：发送完毕，给 C++ 引擎一个特殊的【终止信号】 (空数组)
                    print("[LLM Server] 本次回答结束，下发终止符。")
                    f.write("[]\n")
                    f.flush()
                    
        except Exception as e:
            print(f"[LLM Server] [!] 连接异常: {e}")

if __name__ == '__main__':
    run_server()