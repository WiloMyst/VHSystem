"""
NLP 声学映射微服务

将 C++ 引擎下发的文本块映射为 TTS 模型所需的音素 ID 序列。
使用 piper-phonemize 进行中文分词注音, 通过 TCP 持久连接与主引擎通信。

通信协议:
  请求: 文本行 (UTF-8, 以 \\n 结尾)
  响应: JSON 数组 (音素 ID 列表, 以 \\n 结尾)

运行方式:
  python nlp_server.py
  (需先安装 piper-phonemize 并准备好 TTS 模型的 .onnx.json 配置文件)
"""

import socket
import json
import piper_phonemize


def load_config(config_path: str) -> dict:
    with open(config_path, 'r', encoding='utf-8') as f:
        return json.load(f)


def text_to_ids(text: str, config: dict) -> list:
    """将文本映射为音素 ID 序列"""
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

    print("[NLP] Acoustic mapping service listening on 127.0.0.1:50052")

    while True:
        conn, addr = server.accept()
        print(f"[NLP] Connection established: {addr}")

        try:
            with conn, conn.makefile('rw', encoding='utf-8') as f:
                while True:
                    data = f.readline()
                    if not data:
                        print(f"[NLP] Connection closed by peer: {addr}")
                        break

                    text_chunk = data.strip()
                    if not text_chunk:
                        continue

                    ids = text_to_ids(text_chunk, config)

                    f.write(json.dumps(ids) + '\n')
                    f.flush()

        except Exception as e:
            print(f"[NLP] Error: {e}")


if __name__ == '__main__':
    run_server()
