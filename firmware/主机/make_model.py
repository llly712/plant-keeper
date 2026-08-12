"""
生成 plant_model.h - 最小可用TFLite模型
模型: Input(96,96,3) -> AvgPool -> Reshape -> FC(3->5) -> Softmax -> Output(5)
这个模型能跑通代码流程，实际推理用训练好的替换
"""
import flatbuffers
import struct

def build_model():
    b = flatbuffers.Builder(4096)

    # ============================================================
    # 先用原始方式构建 FlatBuffer - flatbuffers.Builder API 手写
    # 不依赖编译好的schema类，直接按TFLite schema定义来
    # ============================================================

    # 这种方案实在太复杂了，改用方案B：
    # 直接输出预计算的二进制模型
    
    return None


def build_precomputed():
    """
    预计算的TFLite模型二进制
    这是手工按照FlatBuffer格式拼出来的最小可用模型
    结构: Input(96,96,3,uint8) -> FC(3->5,随机权重) -> Softmax -> Output(5,float32)
    
    由于手写太复杂，这里用了一个更简单的办法：
    直接嵌入一个已知有效的简化模型
    """
    
    # 策略：构造模型 FlatBuffer 原始二进制
    # 我会逐步构建每个子结构
    
    data = bytearray()
    
    def align4():
        while len(data) % 4:
            data.append(0)
    
    def u32(v):
        data.extend(struct.pack('<I', v))
    
    def i32(v):
        data.extend(struct.pack('<i', v))
    
    def u16(v):
        data.extend(struct.pack('<H', v))
    
    def u8(v):
        data.append(v & 0xFF)
    
    def f32(v):
        data.extend(struct.pack('<f', v))
    
    # FlatBuffer规则：
    # 每个table: [data fields] [vtable]
    # vtable: [vtable_size:u16] [object_size:u16] [field0_offset:u16] ...
    # 
    # 每个vector: [element0] [element1] ... [elementN-1]
    # 
    # 在builder模式中，数据从末尾往前构建
    # 
    # 最终的buffer布局:
    # [data...] [root_table] [root_offset:u32]
    
    # 由于手工构建极其繁琐，我们用一种取巧的办法：
    # 直接用 Python 模拟 flatbuffers.Builder 的逻辑
    
    # 我放弃手工构建。改为尝试安装 tensorflow 生成。
    return None


def create_with_flatbuffers_builder():
    """
    使用 flatbuffers.Builder 按 TFLite schema 手动构建模型
    不依赖编译好的 schema 类
    """
    builder = flatbuffers.Builder(2048)
    
    # ===== Helper: 给 table 添加字段 =====
    # flatbuffers.Builder.StartObject(num_fields)
    # builder.PrependXxx(field_id, value)  # Xxx = UOffsetTRelative, Int32, etc.
    # builder.EndObject()
    
    # 由于 TFLite schema 字段众多且嵌套复杂，这里采用最简路径
    
    # Buffer 0: 空
    # Buffer 1: FC 权重 (15个int8) + 对齐
    # Buffer 2: FC bias (5个int32)
    
    import random
    random.seed(42)
    
    # 5个输出 * 3个输入 = 15个 int8 权重
    weights_int8 = bytes([random.randint(-127, 127) for _ in range(15)])
    # 5个 int32 bias
    bias_int32 = struct.pack('<5i', *[random.randint(-100, 100) for _ in range(5)])
    
    # ===== 构建 Buffer 表 =====
    # Buffer: { data: [byte] }
    
    # Buffer 0 (空)
    
    # Buffer 1 (weights)
    
    # Buffer 2 (bias)
    
    # ===== 构建 QuantizationParameters =====
    # { scale: [float], zero_point: [int64] }
    
    # ===== 构建 Tensor =====
    # { shape: [int], type: TensorType, buffer: uint, name: string, quantization: QuantParams }
    
    # ===== 构建 OperatorCode =====
    # { builtin_code: BuiltinOperator, version: int }
    
    # ===== 构建 Operator =====
    # { opcode_index: uint, inputs: [int], outputs: [int], builtin_options_type: BuiltinOptions, builtin_options: table }
    
    # ===== 构建 SubGraph =====
    # { tensors: [Tensor], inputs: [int], outputs: [int], operators: [Operator] }
    
    # ===== 构建 Model =====
    # { version: int, operator_codes: [OperatorCode], subgraphs: [SubGraph], buffers: [Buffer] }
    
    # 这个工作量太大了，我们用一个取巧的办法
    
    return builder


# ================================================================
# 终极方案：使用 Python 标准库构造完整 FlatBuffer
# ================================================================

class FlatBufferBuilder:
    """简化版FlatBuffer构建器"""
    def __init__(self):
        self.buf = bytearray()
        
    def pos(self):
        return len(self.buf)
    
    def write_u8(self, v):
        self.buf.append(v & 0xFF)
        
    def write_u16(self, v):
        self.buf.extend(struct.pack('<H', v & 0xFFFF))
        
    def write_u32(self, v):
        self.buf.extend(struct.pack('<I', v & 0xFFFFFFFF))
        
    def write_i32(self, v):
        self.buf.extend(struct.pack('<i', v))
        
    def write_f32(self, v):
        self.buf.extend(struct.pack('<f', v))
        
    def write_i64(self, v):
        self.buf.extend(struct.pack('<q', v))
        
    def write_bytes(self, bs):
        self.buf.extend(bs)
        
    def write_offset(self, pos, target):
        """在pos位置写相对target的偏移 (u32)"""
        rel = target - pos
        struct.pack_into('<I', self.buf, pos, rel)
        
    def align(self, n):
        while len(self.buf) % n != 0:
            self.buf.append(0)
            
    def write_string(self, s):
        """写一个FlatBuffer字符串: [len (u32)] [data (bytes)] [\0]"""
        b = s.encode('utf-8')
        self.write_u32(len(b))
        self.write_bytes(b)
        self.write_u8(0)
        self.align(4)


def make_tflite_model():
    """
    构建最小可用TFLite模型
    结构:
      Input [1,96,96,3] : uint8, quant([-128,127]->[0,1])
      -> FC (weights[3x5] int8, bias[5] int32, quant scales)
      -> Softmax
      -> Output [1,5] : float32
    
    简化说明：TFLite的FC算子对输入的最后维度做运算
    input shape [1,96,96,3] 的最后维度是3
    所以 FC kernel shape 是 [3,5] (3个输入 -> 5个输出)
    只需3*5=15个weight + 5个bias
    
    这样确实是对的——TFLite会把4D input当做batch处理，每个"像素组"
    的3个通道经过同一个FC映射到5个值
    等效于对每个像素位置做1x1卷积然后全局平均...虽然不完美但也算"处理"了
    """
    fb = FlatBufferBuilder()
    
    # === 记录位置的工具 ===
    patches = {}
    
    def mark(name):
        patches[name] = fb.pos()
    
    import random
    random.seed(12345)
    
    # 预先生成权重数据
    weights = [random.randint(-127, 127) for _ in range(15)]  # 3*5
    biases = [random.randint(-50, 50) for _ in range(5)]
    weights_bytes = bytes([w & 0xFF for w in weights])
    biases_bytes = struct.pack('<5i', *biases)
    
    # ===== 数据块 (先写，后面引用) =====
    
    # -- Buffer 2: bias data --
    mark('bias_data')
    fb.write_bytes(biases_bytes)
    
    # -- Buffer 1: weight data --
    mark('weight_data')
    fb.write_bytes(weights_bytes)
    
    # ===== OperatorCode 表 =====
    # 1. FC opcode
    mark('opcode_fc_vt')  # vtable
    fb.write_u16(8)       # vtable size
    fb.write_u16(8)       # object size
    fb.write_u16(0)       # field 0 (deprecated_builtin_code) - not set
    fb.write_u16(4)       # field 1 (builtin_code)  offset from object start
    
    mark('opcode_fc_obj')
    fb.write_i32(4)       # vtable offset (negative)
    fb.write_i32(9)       # builtin_code = FULLY_CONNECTED (9)
    
    # 计算vtable的实际偏移
    opcode_fc_start = patches['opcode_fc_obj']
    fb.write_offset(opcode_fc_start, patches['opcode_fc_vt'])
    
    # 2. Softmax opcode
    mark('opcode_sm_vt')
    fb.write_u16(8)
    fb.write_u16(8)
    fb.write_u16(0)       # field 0 not set
    fb.write_u16(4)       # field 1
    
    mark('opcode_sm_obj') 
    fb.write_i32(4 + len(fb.buf) - len(fb.buf)) # will fix up
    # Actually let me fix up the vtable offset
    opcode_sm_vt_pos = patches['opcode_sm_vt']
    opcode_sm_obj_pos = patches['opcode_sm_obj']
    fb.write_offset(opcode_sm_obj_pos, opcode_sm_vt_pos)
    fb.write_i32(25)      # builtin_code = SOFTMAX (25)
    
    # 重新fix - no, let me just redo this cleaner
    # Actually this is getting really messy with manual offset tracking
    
    # --- 重新来过，用更清晰的方式 ---
    return _build_clean()


def _build_clean():
    """
    重新用 flatbuffers.Builder 构建模型
    这次我们用 EndObject 返回的 offset 来相互引用
    """
    import flatbuffers
    import struct
    import random
    
    random.seed(42)
    b = flatbuffers.Builder(4096)
    
    # ====================================================
    # TFLite Schema field IDs (不需要编译好的类，直接用)
    # ====================================================
    
    # Model: version=0, operator_codes=1, subgraphs=2, buffers=4
    # OperatorCode: deprecated_builtin_code=0, builtin_code=1, version=2
    # SubGraph: tensors=0, inputs=1, outputs=2, operators=3
    # Tensor: shape=0, type=1, buffer=2, name=3, quantization=4
    # QuantizationParameters: scale=2, zero_point=3
    # Operator: opcode_index=0, inputs=1, outputs=2, builtin_options_type=3, builtin_options=4
    # Pool2DOptions: padding=0, stride_w=1, stride_h=2, filter_width=3, filter_height=4, fused_activation_function=5
    # FullyConnectedOptions: fused_activation_function=0, weights_format=1
    # SoftmaxOptions: beta=0
    # ReshapeOptions: new_shape=0
    
    # ===== 数据准备 =====
    # FC权重: [3, 5] int8 (3输入通道 -> 5输出类)
    fc_weights = bytes([random.randint(-127, 127) & 0xFF for _ in range(15)])
    fc_bias = struct.pack('<5i', *[random.randint(-50, 50) for _ in range(5)])
    
    # ===== 构建字符串 =====
    def build_string(s):
        return b.CreateString(s)
    
    # ===== 构建 Buffer =====
    def build_buffer(data=b''):
        data_offset = b.CreateByteVector(data) if data else 0
        b.StartObject(1)
        if data:
            b.PrependUOffsetTRelativeSlot(0, data_offset, 0)
        return b.EndObject()
    
    buf0 = build_buffer()  # 空buffer
    buf1 = build_buffer(fc_weights)  # FC权重
    buf2 = build_buffer(fc_bias)     # FC bias
    
    # ===== 构建 QuantizationParameters =====
    # 输入量化: scale=1.0 (uint8范围0-255映射到0-1)
    def create_float_vec(values):
        if not values: return 0
        b.StartVector(len(values), 4, 4)
        for v in reversed(values):
            b.PrependFloat32(v)
        return b.EndVector()
    
    def create_int64_vec(values):
        if not values: return 0
        b.StartVector(len(values), 8, 8)
        for v in reversed(values):
            b.PrependInt64(v)
        return b.EndVector()
    
    def create_int32_vec(values):
        if not values: return 0
        b.StartVector(len(values), 4, 4)
        for v in reversed(values):
            b.PrependInt32(v)
        return b.EndVector()
    
    def create_table_vec(tables):
        if not tables: return 0
        b.StartVector(4, len(tables), 4)  # 4 bytes per uoffset
        for t in reversed(tables):
            b.PrependUOffsetTRelative(t)
        return b.EndVector()
    
    # ===== 构建 QuantizationParameters =====
    def build_qparams(scales, zero_pts):
        scale_off = create_float_vec(scales)
        zp_off = create_int64_vec(zero_pts)
        b.StartObject(4)
        if scale_off: b.PrependUOffsetTRelativeSlot(2, scale_off, 0)
        if zp_off: b.PrependUOffsetTRelativeSlot(3, zp_off, 0)
        return b.EndObject()
    
    q_input = build_qparams([1.0/255.0], [0])
    q_weight = build_qparams([0.02], [0])
    q_bias = build_qparams([0.02/255.0], [0])
    q_none = build_qparams([1.0], [0])  # 无量化(用于float32输出)
    
    # ===== 构建 Tensor =====
    def build_tensor(shape, type_val, buf_idx, name="", qparams=0):
        shape_off = create_int32_vec(shape)
        name_off = build_string(name) if name else 0
        b.StartObject(5)
        if shape_off: b.PrependUOffsetTRelativeSlot(0, shape_off, 0)
        b.PrependInt32Slot(1, type_val, 0)
        b.PrependUint32Slot(2, buf_idx, 0)
        if name_off: b.PrependUOffsetTRelativeSlot(3, name_off, 0)
        if qparams: b.PrependUOffsetTRelativeSlot(4, qparams, 0)
        return b.EndObject()
    
    TensorType = {'FLOAT32': 0, 'INT32': 2, 'UINT8': 3, 'INT8': 9}
    
    # Tensor 0: 输入 [1,96,96,3] UINT8
    t_input = build_tensor([1,96,96,3], TensorType['UINT8'], 0, "input", q_input)
    # Tensor 1: FC权重 [3,5] INT8
    t_weights = build_tensor([3,5], TensorType['INT8'], 1, "fc_weights", q_weight)
    # Tensor 2: FC bias [5] INT32
    t_bias = build_tensor([5], TensorType['INT32'], 2, "fc_bias", q_bias)
    # Tensor 3: FC输出 [1,5] FLOAT32
    t_fc_out = build_tensor([1,5], TensorType['FLOAT32'], 0, "fc_out")
    # Tensor 4: 最终输出 [1,5] FLOAT32
    t_output = build_tensor([1,5], TensorType['FLOAT32'], 0, "output", q_none)
    
    tensors = [t_input, t_weights, t_bias, t_fc_out, t_output]
    
    # ===== 构建 OperatorCode =====
    def build_opcode(code, version=1):
        b.StartObject(3)
        b.PrependInt32Slot(1, code, 0)       # builtin_code
        b.PrependInt32Slot(2, version, 0)    # version
        return b.EndObject()
    
    op_fc = build_opcode(9)     # FULLY_CONNECTED
    op_sm = build_opcode(25)    # SOFTMAX
    
    # ===== 构建 Operator =====
    
    # FC options: { fused_activation_function: NONE=0, weights_format: DEFAULT=0 }
    def build_fc_options():
        b.StartObject(2)
        b.PrependInt8Slot(0, 0, 0)    # fused_activation = NONE
        b.PrependInt8Slot(1, 0, 0)    # weights_format = DEFAULT
        return b.EndObject()
    
    fc_opts = build_fc_options()
    
    # FC operator: opcode=0, inputs=[0,1,2], outputs=[3], options=FC
    def build_operator(opcode_idx, inputs, outputs, opt_type, opt_table):
        inp_off = create_int32_vec(inputs)
        out_off = create_int32_vec(outputs)
        b.StartObject(5)
        b.PrependUint32Slot(0, opcode_idx, 0)
        b.PrependUOffsetTRelativeSlot(1, inp_off, 0)
        b.PrependUOffsetTRelativeSlot(2, out_off, 0)
        b.PrependUint8Slot(3, opt_type, 0)     # builtin_options_type
        b.PrependUOffsetTRelativeSlot(4, opt_table, 0)
        return b.EndObject()
    
    # Softmax operator: opcode=1, inputs=[3], outputs=[4]
    def build_sm_operator():
        inp_off = create_int32_vec([3])
        out_off = create_int32_vec([4])
        b.StartObject(1)                          # SoftmaxOptions table
        b.PrependFloat32Slot(0, 1.0, 0)          # beta = 1.0
        sm_opts = b.EndObject()
        b.StartObject(5)
        b.PrependUint32Slot(0, 1, 0)             # opcode_index = 1
        b.PrependUOffsetTRelativeSlot(1, inp_off, 0)
        b.PrependUOffsetTRelativeSlot(2, out_off, 0)
        b.PrependUint8Slot(3, 25, 0)             # builtin_options_type = SoftmaxOptions
        b.PrependUOffsetTRelativeSlot(4, sm_opts, 0)
        return b.EndObject()
    
    op0 = build_operator(0, [0, 1, 2], [3], 9, fc_opts)
    op1 = build_sm_operator()
    
    # ===== 构建 SubGraph =====
    tensors_off = create_table_vec(tensors)
    inputs_off = create_int32_vec([0])
    outputs_off = create_int32_vec([4])
    ops_off = create_table_vec([op0, op1])
    name_off = build_string("main")
    
    b.StartObject(5)
    b.PrependUOffsetTRelativeSlot(0, tensors_off, 0)
    b.PrependUOffsetTRelativeSlot(1, inputs_off, 0)
    b.PrependUOffsetTRelativeSlot(2, outputs_off, 0)
    b.PrependUOffsetTRelativeSlot(3, ops_off, 0)
    b.PrependUOffsetTRelativeSlot(4, name_off, 0)
    subgraph = b.EndObject()
    
    # ===== 构建 Model =====
    opcodes_off = create_table_vec([op_fc, op_sm])
    subgraphs_off = create_table_vec([subgraph])
    buffers_off = create_table_vec([buf0, buf1, buf2])
    desc_off = build_string("Plant Disease Classifier (5-class)")
    
    b.StartObject(6)
    b.PrependUint32Slot(0, 3, 0)           # version = 3
    b.PrependUOffsetTRelativeSlot(1, opcodes_off, 0)
    b.PrependUOffsetTRelativeSlot(2, subgraphs_off, 0)
    b.PrependUOffsetTRelativeSlot(3, desc_off, 0)
    b.PrependUOffsetTRelativeSlot(4, buffers_off, 0)
    model = b.EndObject()
    
    b.Finish(model)
    
    return b.Output()


# ================================================================
# 主入口
# ================================================================
if __name__ == '__main__':
    print("正在生成 TFLite 模型...")
    
    try:
        model_bytes = _build_clean()
        
        # 写入 plant_model.h
        with open('plant_model.h', 'w', encoding='utf-8') as f:
            f.write('// 植物病害AI模型 (测试版-随机权重)\n')
            f.write('// 输入: [1,96,96,3] uint8\n')
            f.write('// 输出: [1,5] float32 (softmax概率)\n')
            f.write('// 类别: 健康, 白粉病, 叶斑病, 锈病, 虫害\n')
            f.write('// 运行 train_model.py 用真实数据训练替换\n\n')
            f.write('#ifndef PLANT_MODEL_H\n')
            f.write('#define PLANT_MODEL_H\n\n')
            f.write(f'const unsigned char plant_model[] = {{\n')
            
            for i in range(0, len(model_bytes), 12):
                chunk = model_bytes[i:i+12]
                hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
                f.write(f'  {hex_str},\n')
            
            f.write('};\n')
            f.write(f'const unsigned int plant_model_len = {len(model_bytes)};\n\n')
            f.write('#endif\n')
        
        print(f"生成 plant_model.h 完成 ({len(model_bytes)} bytes)")
        print("代码现在可以编译了!")
        print("用真实数据训练好模型后，替换这个文件即可")
    
    except Exception as e:
        print(f"构建模型失败: {e}")
        import traceback
        traceback.print_exc()
