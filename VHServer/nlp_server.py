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
    # 确保此处的 json 路径与你实际存放的路径一致
    config = load_config("models/zh_CN-huayan-medium.onnx.json")
    
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # 允许端口复用
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('127.0.0.1', 50052))
    server.listen(50) # 支持高并发 backlog
    
    print("[NLP Server] 纯文本解析微服务已启动，监听 127.0.0.1:50052...")

    while True:
        conn, addr = server.accept()
        try:
            data = conn.recv(4096).decode('utf-8')
            if not data:
                continue
            
            ids = text_to_ids(data, config)
            response = json.dumps(ids)
            
            conn.sendall(response.encode('utf-8'))
        except Exception as e:
            print(f"[NLP Server] Error: {e}")
        finally:
            conn.close()

if __name__ == '__main__':
    run_server()