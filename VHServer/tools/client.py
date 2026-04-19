# 仅用于测试的文件
import sys
import os
import grpc
import time
import uuid
import wave
import json
import piper_phonemize # 注音神器

# 把 protos 目录加进路径
sys.path.append(os.path.join(os.path.dirname(__file__), 'protos'))

import message_pb2
import message_pb2_grpc

# =====================================================================
# 核心逻辑：加载配置，把人类文本变成机器 ID
# =====================================================================
def text_to_phoneme_ids(text, config_path):
    with open(config_path, 'r', encoding='utf-8') as f:
        config = json.load(f)
    id_map = config['phoneme_id_map']

    # 1. 转换为音素符号列表
    phonemes_list = piper_phonemize.phonemize_espeak(text, "cmn")
    
    # 2. 企业级兼容获取特殊标识符 ID (防崩溃)
    # 如果根目录没有 'bos'，就去 id_map 里找 '^'，如果还没有，就默认给 1
    bos_id = config.get('bos') or id_map.get('^', [1])[0]
    eos_id = config.get('eos') or id_map.get('$', [2])[0]
    pad_id = config.get('pad') or id_map.get('_', [0])[0]
    
    # 3. 拼接最终的张量 ID 数组
    phoneme_ids = []
    phoneme_ids.append(bos_id)
    
    for phonemes in phonemes_list:
        for p in phonemes:
            if p in id_map:
                phoneme_ids.extend(id_map[p])
                phoneme_ids.append(pad_id)
                
    phoneme_ids.append(eos_id)
    
    return phoneme_ids

# =====================================================================
# 交互式发包生成器
# =====================================================================
def interactive_stream(session_id, config_path):
    print("\n" + "="*50)
    print("欢迎来到双模态引擎中枢控制台")
    print("请输入任意中文句子 (输入 'q' 退出)")
    print("="*50 + "\n")
    
    msg_id = 0
    while True:
        user_input = input(f"[{session_id}] 请输入文本 ❯ ")
        if user_input.lower() == 'q':
            break
        if not user_input.strip():
            continue
            
        # 在 Python 端完成复杂的 NLP 计算，转为 ID 数组
        ids = text_to_phoneme_ids(user_input, config_path)
        print(f"[前端计算] 提取音素 IDs: {ids}")
        
        # 封装进 gRPC
        req = message_pb2.AvatarStreamRequest(
            session_id=session_id,
            stream_type="TEXT_INFER",
            phoneme_ids=ids, # 这里使用了刚才 proto 里新增的字段！
            is_end_of_stream=False
        )
        
        msg_id += 1
        yield req

def run():
    # 确保刚才下载的 config json 文件路径正确
    config_path = "models/zh_CN-huayan-medium.onnx.json" 
    
    channel = grpc.insecure_channel('localhost:50051')
    stub = message_pb2_grpc.AvatarEngineStub(channel)
    session_id = uuid.uuid4().hex[:8]
    
    try:
        response_stream = stub.ChatWithAvatar(interactive_stream(session_id, config_path))
        
        # 监听服务端的回包
        for i, response in enumerate(response_stream):
            if response.success:
                print(f"[<- 引擎返回] 音频流极速生成完毕！({len(response.audio_pcm)} bytes)")
                
                # 每回答一句话，存一个波形文件
                out_filename = f"output_chat_{i}.wav"
                with wave.open(out_filename, 'wb') as wav_file:
                    wav_file.setnchannels(1)
                    wav_file.setsampwidth(2) 
                    wav_file.setframerate(22050)
                    wav_file.writeframes(response.audio_pcm)
                print(f"已保存音频至当前目录: {out_filename}\n")
            else:
                print(f"[<- 报错] 服务端异常: {response.error_msg}")
                
    except grpc.RpcError as e:
        print(f"链路崩溃: 状态码={e.code()}")

if __name__ == '__main__':
    # python3 -m grpc_tools.protoc -I./protos --python_out=./protos --grpc_python_out=./protos ./protos/avatarStream.proto
    run()