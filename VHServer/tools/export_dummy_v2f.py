import torch
import torch.nn as nn
import torch.nn.functional as F
import os
import onnx

class SmartDummyV2F(nn.Module):
    def __init__(self, fps=30, sample_rate=22050):
        super().__init__()
        # 1. 定义帧长：22050 采样率下，30帧/秒意味着每帧需要 735 个采样点
        self.frame_size = sample_rate // fps  
        
        # 2. 映射层：把单维度的“声音能量”映射到 52 维的面部骨骼上
        self.mapper = nn.Linear(1, 52, bias=False)
        
        # 【黑魔法初始化】：切断默认的随机抽搐
        nn.init.zeros_(self.mapper.weight)
        
        with torch.no_grad():
            # 索引 17 是 JawOpen (下巴张开 / 嘴巴张开核心)
            self.mapper.weight[17, 0] = 1.2  # 稍微加大张嘴幅度
            
            # 索引 18 是 mouthClose (闭嘴)
            self.mapper.weight[18, 0] = -0.5 # 声音大时确保嘴唇分开
            
            # 索引 23, 24 是微笑 (增加一点随意的生动感)
            self.mapper.weight[23, 0] = 0.2  # mouthSmileLeft
            self.mapper.weight[24, 0] = 0.2  # mouthSmileRight

    def forward(self, audio_pcm):
        # 此时 audio_pcm shape: [batch_size, seq_len]
        
        # 1. 扩充通道维度，适应 1D 池化 -> [batch_size, 1, seq_len]
        x = audio_pcm.unsqueeze(1)
        
        # 2. 计算绝对幅度 (声音响度)
        x_abs = torch.abs(x)
        
        # 3. 核心：局部特征提取！使用平均池化将声音按 "帧" 压缩
        # 输出 shape: [batch_size, 1, num_frames]
        energy_frames = F.avg_pool1d(x_abs, kernel_size=self.frame_size, stride=self.frame_size)
        
        # 4. 放大能量信号（因为平均能量通常只有 0.1 左右），并截断在 0~1 之间
        energy_frames = torch.clamp(energy_frames * 6.0, 0.0, 1.0)
        
        # 5. 翻转维度适应 Linear 层 -> [batch_size, num_frames, 1]
        energy_frames = energy_frames.transpose(1, 2)
        
        # 6. 映射到 52 维表情 -> [batch_size, num_frames, 52]
        blendshapes = self.mapper(energy_frames)
        
        # 再次确保数值绝对安全
        blendshapes = torch.clamp(blendshapes, 0.0, 1.0)
        
        return blendshapes

# =================================================================
# 执行导出
# =================================================================
model = SmartDummyV2F()
model.eval()

# 我们用一段 2 秒钟的虚拟音频来做导出契约 (1 * 44100 个采样点)
dummy_input = torch.randn(1, 44100)
onnx_path = "./models/dummy_v2f.onnx"

print("正在导出【时间序列架构】的 V2F 挡板模型 (Opset=13)...")
torch.onnx.export(
    model, 
    dummy_input, 
    onnx_path,
    export_params=True,
    opset_version=13,
    input_names=['audio_pcm'],
    output_names=['blendshapes'],
    dynamic_axes={
        # 核心结构：输入是动态音频长度，输出是动态【帧数】序列！
        'audio_pcm': {0: 'batch_size', 1: 'seq_len'}, 
        'blendshapes': {0: 'batch_size', 1: 'num_frames'} 
    }
)

print("正在覆写 IR 版本为 8...")
onnx_model = onnx.load(onnx_path)
onnx_model.ir_version = 8
onnx.save(onnx_model, onnx_path)

print("\n" + "="*50)
print(f"成功！带有口型联动的序列化 V2F 模型已生成: {onnx_path}")
print("="*50 + "\n")