import socket
import json
import piper_phonemize # 注音

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
    
    print("[NLP Server] 纯文本解析微服务已启动，监听 127.0.0.1:50052...")

    while True:
        conn, addr = server.accept()
        print(f"[NLP Server] [+] 建立持久长连接: {addr}")
        
        try:
            # 神级操作：将原生 socket 包装成文件流 (makefile)，完美解决 TCP 粘包问题！
            with conn, conn.makefile('rw', encoding='utf-8') as f:
                while True: # 开启长连接心跳循环
                    data = f.readline() # 按行读取，只要没读到换行符就阻塞等待
                    if not data:
                        print(f"[NLP Server] [-] 客户端已主动断开连接: {addr}")
                        break 
                    
                    text = data.strip()
                    if not text:
                        continue
                    
                    # 执行 NLP 推理
                    ids = text_to_ids(text, config)
                    response = json.dumps(ids)
                    
                    # 结果加上换行符，推回给 C++ 端
                    f.write(response + '\n')
                    f.flush() # 强制立刻刷入网卡缓冲
                    
        except Exception as e:
            print(f"[NLP Server] [!] 连接异常断开: {e}")

if __name__ == '__main__':
    run_server()