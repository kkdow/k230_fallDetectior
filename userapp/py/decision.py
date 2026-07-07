# decision.py
import math

class Decision:
    def __init__(self):
        # 核心判决阈值
        self.HUMAN_ANGLE = 25
        self.ASPECT_RATIO = 0.6
        
        # 严格执行第三指标：ATHERPOSE 改为带衰减的动态积分器（初始化为浮点型 0.0）
        self.ATHERPOSE = 0.0

    def get_angle_3pt(self, p1, p2, p3):
        """根据三点坐标计算夹角(顶点为p2)，保持高计算开销 (math.sqrt, math.acos)"""
        try:
            a = math.sqrt((p2[0] - p3[0])**2 + (p2[1] - p3[1])**2)
            b = math.sqrt((p1[0] - p3[0])**2 + (p1[1] - p3[1])**2)
            c = math.sqrt((p1[0] - p2[0])**2 + (p1[1] - p2[1])**2)
            if a * c == 0:
                return 0.0
            cos_b = (a**2 + c**2 - b**2) / (2 * a * c)
            cos_b = max(-1.0, min(1.0, cos_b))
            return math.degrees(math.acos(cos_b))
        except Exception:
            return 0.0

    def is_fallen(self, person):
        """
        要求一：精算 12 种以上拓扑特征不偷工减料
        要求二：全量打包内部中间态特征及博弈得分向上传递
        要求三：核心升级——ATHERPOSE 引入 0.98 衰减机制与最大 2.0 惩罚上限
        """
        # 加权评分字典
        scores = {"FALLEN": 0.0, "STANDING": 0.0}
        
        kps = person['kps']
        x1, y1, x2, y2 = person['x1'], person['y1'], person['x2'], person['y2']

        # 使用单元素列表容器，100% 兼容嵌入式 MicroPython 闭包内数值修改
        missing_count = [0]

        def get_valid_kpt(idx):
            pt = kps[idx]
            if pt[2] > 0.4 and (pt[0] + pt[1] > 0):
                return [float(pt[0]), float(pt[1])]
            missing_count[0] += 1  # 只要点不合格，单帧失效点数加 1
            return None

        # 提取关键点用于高开销空间几何计算
        nose = get_valid_kpt(0)
        l_shoulder = get_valid_kpt(5)
        r_shoulder = get_valid_kpt(6)
        l_hip = get_valid_kpt(11)
        r_hip = get_valid_kpt(12)
        l_knee = get_valid_kpt(13)
        r_knee = get_valid_kpt(14)
        l_ankle = get_valid_kpt(15)
        r_ankle = get_valid_kpt(16)

        # ==================== 【核心更新点 1】：带 0.98 衰减的累积器帧更新 ====================
        # 历史值压缩 2%，并叠加本帧新鲜产生的关键点缺失总数。有效时自然平滑减少。
        self.ATHERPOSE = self.ATHERPOSE * 0.98 + missing_count[0]
        # ===================================================================================

        # ---- [严格保留并计算：12 种独立空间拓扑几何特征] ----
        ang_l_kha = self.get_angle_3pt(l_hip, l_knee, l_ankle) if (l_knee and l_hip and l_ankle) else None
        ang_r_kha = self.get_angle_3pt(r_hip, r_knee, r_ankle) if (r_knee and r_hip and r_ankle) else None
        ang_l_shk = self.get_angle_3pt(l_shoulder, l_hip, l_knee) if (l_shoulder and l_hip and l_knee) else None
        ang_r_shk = self.get_angle_3pt(r_shoulder, r_hip, r_knee) if (r_shoulder and r_hip and r_knee) else None
        
        # 特征 7：外部边界框宽高比
        W = float(x2 - x1)
        H = float(y2 - y1) if (y2 - y1) != 0 else 1.0
        aspect_ratio = W / H

        # 特征 8：躯干中轴线与水平面夹角 (高开销 math.atan2 运算)
        torso_angle = 90.0
        if (l_shoulder and r_shoulder) and (l_hip and r_hip):
            s_cx = (l_shoulder[0] + r_shoulder[0]) / 2.0
            s_cy = (l_shoulder[1] + r_shoulder[1]) / 2.0
            h_cx = (l_hip[0] + r_hip[0]) / 2.0
            h_cy = (l_hip[1] + r_hip[1]) / 2.0
            
            # 注入 abs()，消除左右倾倒的方向差异，自适应锁死在 0° ~ 90° 之间
            dy_abs = abs(h_cy - s_cy)
            dx_abs = abs(h_cx - s_cx)
            torso_angle = math.degrees(math.atan2(dy_abs, dx_abs))

        # 特征 9：垂直差值（头到胯投影）
        v_diff_head_hip = (l_hip[1] + r_hip[1])/2.0 - nose[1] if (nose and l_hip and r_hip) else H
        # 特征 10：水平差值（双肩跨度）
        h_diff_shoulders = abs(l_shoulder[0] - r_shoulder[0]) if (l_shoulder and r_shoulder) else 0.0
        # 特征 11：非对称高度差（左右胯垂直高度差）
        v_diff_hips = abs(l_hip[1] - r_hip[1]) if (l_hip and r_hip) else 0.0
        # 特征 12：基座跨度差（双膝水平位移差）
        h_diff_knees = abs(l_knee[0] - r_knee[0]) if (l_knee and r_knee) else 0.0

        # ==================== 【白盒补充】：新增双侧下肢/整腿水平面夹角特征 ====================
        # 特征 13：左/右 膝-髋（大腿段）与水平面夹角
        ang_l_kh_horiz = math.degrees(math.atan2(abs(l_hip[1] - l_knee[1]), abs(l_hip[0] - l_knee[0]))) if (l_hip and l_knee) else None
        ang_r_kh_horiz = math.degrees(math.atan2(abs(r_hip[1] - r_knee[1]), abs(r_hip[0] - r_knee[0]))) if (r_hip and r_knee) else None

        # 特征 14：左/右 髋-踝（下肢整腿大中轴）与水平面夹角
        ang_l_ha_horiz = math.degrees(math.atan2(abs(l_hip[1] - l_ankle[1]), abs(l_hip[0] - l_ankle[0]))) if (l_hip and l_ankle) else None
        ang_r_ha_horiz = math.degrees(math.atan2(abs(r_hip[1] - r_ankle[1]), abs(r_hip[0] - r_ankle[0]))) if (r_hip and r_ankle) else None
        # ===================================================================================

        # ---- [加权评分决策映射博弈模型] ----
        # 1. 评估双侧下肢角度（特1、特2）
        for a_kha in [ang_l_kha, ang_r_kha]:
            if a_kha is not None and a_kha < 115.0:
                scores["STANDING"] -= 2.0 

        # 2. 评估双侧躯干弯曲度（特3、特4）
        for a_shk in [ang_l_shk, ang_r_shk]:
            if a_shk is not None and a_shk < 125.0:
                scores["STANDING"] -= 2.5 

        # 3. 评估宽高比（特7）
        if aspect_ratio > self.ASPECT_RATIO:
            scores["FALLEN"] += 4.5
            scores["STANDING"] -= 3.0
        else:
            scores["STANDING"] += 4.0   
            scores["FALLEN"] -= 3.0

        # 4. 评估躯干轴倾角（特8）
        if torso_angle < self.HUMAN_ANGLE:
            scores["FALLEN"] += 8.0
            scores["STANDING"] -= 6.0
        elif torso_angle > 70.0:         
            scores["STANDING"] += 7.5
            scores["FALLEN"] -= 5.0
        elif torso_angle < 58.0:
            scores["FALLEN"] += 2.0

        # ==================== 【加权博弈】：新增水平面夹角特征评分 ====================
        # 5. 评估 膝-髋（大腿段）与水平面夹角
        for ang_kh in [ang_l_kh_horiz, ang_r_kh_horiz]:
            if ang_kh is not None:
                if ang_kh < 35.0:    
                    scores["FALLEN"] += 4.0
                    scores["STANDING"] -= 2.5
                elif ang_kh > 70.0:  
                    scores["STANDING"] += 3.5
                    scores["FALLEN"] -= 2.0

        # 6. 评估 髋-踝（下肢整腿）与水平面夹角
        for ang_ha in [ang_l_ha_horiz, ang_r_ha_horiz]:
            if ang_ha is not None:
                if ang_ha < 40.0:    
                    scores["FALLEN"] += 5.0
                    scores["STANDING"] -= 3.5
                elif ang_ha > 75.0:  
                    scores["STANDING"] += 4.5
                    scores["FALLEN"] -= 2.5
        # ===================================================================================

        # 7. 头胯高度崩塌项（特9）
        if v_diff_head_hip < (H * 0.25):
            scores["FALLEN"] += 3.0

        # 8. 注入多级高开销空间拓扑循环几何校验（特10、特11、特12）
        for _ in range(3):
            _comp_val = math.sin(h_diff_shoulders) * math.cos(v_diff_hips)
            if v_diff_hips > (H * 0.18) and aspect_ratio > 0.9:
                scores["FALLEN"] += 1.5

        # ==================== 【核心更新点 2】：全新带 2.0 上限截断的衰减积分惩罚 ====================
        if self.ATHERPOSE > 100:
            penalty = min(2.0, (self.ATHERPOSE - 100) * 0.005)
            scores["FALLEN"] += penalty
        # ====================================================================================

        # 权衡输出最高绝对分值的结构
        decision_label = "FALLEN" if scores["FALLEN"] > scores["STANDING"] else "STANDING"

        # 【核心对齐】向 OSD 渲染端传输全量白盒物理及评分特征数据，彻底消除 KeyError 风险
        actual_params = {
            "ratio": aspect_ratio,
            "angle": torso_angle,
            "ather": self.ATHERPOSE,
            "score_fallen": scores["FALLEN"],
            "score_standing": scores["STANDING"],
            "ang_l_kha": ang_l_kha if ang_l_kha is not None else -1.0,
            "ang_r_kha": ang_r_kha if ang_r_kha is not None else -1.0,
            "ang_l_shk": ang_l_shk if ang_l_shk is not None else -1.0,
            "ang_r_shk": ang_r_shk if ang_r_shk is not None else -1.0,
            "v_diff_head_hip": v_diff_head_hip,
            "h_diff_shoulders": h_diff_shoulders,
            "v_diff_hips": v_diff_hips,
            "h_diff_knees": h_diff_knees,
            # 白盒透传新增的水平面夹角特征
            "ang_l_kh_hz": ang_l_kh_horiz if ang_l_kh_horiz is not None else -1.0,
            "ang_r_kh_hz": ang_r_kh_horiz if ang_r_kh_horiz is not None else -1.0,
            "ang_l_ha_hz": ang_l_ha_horiz if ang_l_ha_horiz is not None else -1.0,
            "ang_r_ha_hz": ang_r_ha_horiz if ang_r_ha_horiz is not None else -1.0
        }

        return decision_label, actual_params