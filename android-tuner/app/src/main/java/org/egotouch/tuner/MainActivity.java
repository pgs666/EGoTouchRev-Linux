package org.egotouch.tuner;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Bundle;
import android.os.Environment;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;
import androidx.activity.EdgeToEdge;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.bottomnavigation.BottomNavigationView;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.chip.Chip;
import com.google.android.material.chip.ChipGroup;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.slider.Slider;
import com.google.android.material.snackbar.Snackbar;
import com.google.android.material.switchmaterial.SwitchMaterial;
import com.google.android.material.tabs.TabLayout;
import com.google.android.material.textfield.TextInputEditText;
import com.google.android.material.textfield.TextInputLayout;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;

public final class MainActivity extends AppCompatActivity {
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile Map<String, Integer> values = Collections.emptyMap();
    private final AtomicInteger operationGeneration = new AtomicInteger();
    private volatile boolean destroyed;
    private boolean tunerVisible = true;
    private FrameLayout content;
    private MaterialToolbar toolbar;
    private String algoPath;
    private volatile String connectionMessage = "正在请求 root 权限…";
    private volatile boolean connectionFailed;
    private volatile boolean connectionBusy = true;
    private volatile boolean diagnosticChecked;
    private volatile boolean diagnosticSupported;
    private volatile boolean resetSupported;
    private volatile String diagnosticMessage = "正在检查当前内核的诊断能力…";
    private String lastCaptureDir = Environment.getExternalStorageDirectory().getPath() + "/HimaxTouchDiag";

    private static final class Param {
        final String group, key, title, description;
        final int defaultValue, min, max, step;
        final boolean toggle, entry;
        Param(String group, String key, String title, String description, int def, int min, int max, int step, boolean toggle, boolean entry) {
            this.group=group; this.key=key; this.title=title; this.description=description;
            this.defaultValue=def; this.min=min; this.max=max; this.step=step; this.toggle=toggle; this.entry=entry;
        }
    }
    private static Param t(String g,String k,String n,String d,int v){return new Param(g,k,n,d,v,0,1,1,true,false);}
    private static Param s(String g,String k,String n,String d,int v,int lo,int hi,int step){return new Param(g,k,n,d,v,lo,hi,step,false,false);}
    private static Param e(String g,String k,String n,String d,int v){return new Param(g,k,n,d,v,0,0,1,false,true);}

    private static final List<Param> PARAMS = Arrays.asList(
        t("预处理","baseline_enabled","自适应基线","逐像素追踪温漂与 VCOM 漂移，触摸区域自动冻结",1),
        s("预处理","baseline_noise_deadband","基线噪声死区","基线附近按噪声慢速跟踪的范围",90,0,200,1),
        s("预处理","baseline_peak_threshold","基线冻结阈值","高于此差值视为真实触摸并冻结基线",305,100,1000,5),
        s("预处理","baseline_release_hold_frames","基线释放保持帧","抬手后暂缓吸收负回弹",60,0,120,1),
        t("预处理","cmf_enabled","CMF 开关","消除帧间共模噪声",1),
        s("预处理","cmf_exclusion","CMF 排除阈值","触点不计入 CMF 均值的阈值",2000,50,3000,10),
        s("预处理","cmf_max_correction","CMF 最大修正量","每行列的修正上限",2000,50,3000,10),
        t("预处理","iir_enabled","IIR 开关","IIR 时域滤波",1),
        s("预处理","iir_decay_weight","IIR 衰减权重","0 为全历史，256 为不混合",200,0,256,1),
        s("预处理","iir_decay_step","IIR 衰减步长","每帧信号自然衰减量",80,0,200,1),
        e("预处理","iir_noise_floor","IIR 噪声底","低于此值的信号清零",5),
        s("预处理","iir_gate_floor","IIR 门限底","动态门限最低值",200,20,500,5),
        e("预处理","iir_gate_ratio_q8","IIR 门限比率 (Q8)","动态门限比率",26),
        s("预处理","baseline_background_alpha_shift","背景基线更新速率","移位越大越慢",3,0,8,1),
        s("预处理","baseline_no_finger_alpha_shift","无指基线更新速率","无手指时的平滑系数",3,0,8,1),
        s("预处理","baseline_recovery_alpha_shift","恢复基线更新速率","恢复状态更新速率",2,0,8,1),
        s("预处理","baseline_background_max_step","背景基线最大步长","背景状态单帧最大修正",512,0,1024,32),
        s("预处理","baseline_no_finger_max_step","无指基线最大步长","无指状态单帧最大修正",512,0,1024,32),
        s("预处理","baseline_recovery_max_step","恢复基线最大步长","恢复状态单帧最大修正",256,0,512,32),
        s("预处理","baseline_recovery_max_frames","恢复状态最大帧数","恢复状态最长持续帧",30,0,120,1),
        t("预处理","baseline_noise_tracking","基线噪声跟踪","允许死区内慢速跟踪",1),

        s("检测","macro_threshold","宏区阈值","BFS 种子像素最低信号",280,100,2000,10),
        s("检测","peak_threshold","峰值阈值","峰值候选最低信号",280,100,2000,10),
        t("检测","palm_enabled","手掌拒绝","过滤异常大面积接触",1),
        s("检测","palm_area_threshold","手掌面积阈值","像素数达到此值判为手掌",50,5,150,1),
        e("检测","palm_signal_threshold","手掌信号阈值","信号总和判定阈值",80000),
        s("检测","palm_density_low","手掌低密度阈值","信号面积比阈值",400,50,800,10),
        s("检测","peak_local_radius","峰值局部半径","局部极大值邻域",1,1,5,1),
        t("检测","peak_z8_enabled","峰值 Z8 检测","过滤孤立尖峰",1),
        t("检测","peak_saddle_enabled","鞍点检测","分离邻近触点",1),
        s("检测","peak_saddle_radius","鞍点检测半径","鞍点搜索邻域",2,1,8,1),
        s("检测","peak_saddle_drop","鞍点下降阈值","拆分双峰的最小落差",80,0,256,8),
        s("检测","peak_signal_threshold_limit","压力漂移信号上限","压力漂移独立阈值",1000,100,2000,10),
        s("检测","peak_edge_threshold","峰值边缘阈值","边缘峰值信号门槛",300,100,1000,10),
        s("检测","peak_macro_min_area","峰值宏区最小面积","过滤孤立噪点",3,1,20,1),
        t("检测","palm_box_enabled","手掌框拒绝","以包围盒识别手掌",1),
        s("检测","palm_box_expand_rows","手掌框扩展行数","行方向扩展",9,0,10,1),
        s("检测","palm_box_expand_cols","手掌框扩展列数","列方向扩展",10,0,10,1),
        s("检测","palm_box_match_distance","手掌框匹配距离","相邻帧最大距离",6,0,20,1),
        s("检测","palm_box_max_hold","手掌框保持帧数","无匹配最大保持",0,0,60,1),
        t("检测","zone_cleanup_enabled","区域清理","去除触点周围杂散像素",1),
        s("检测","zone_max_radius","区域最大半径","区域清理搜索半径",3,0,16,1),
        s("检测","zone_threshold_numer","区域阈值分子","区域阈值定点分子",64,0,255,1),
        s("检测","zone_threshold_shift","区域阈值移位","区域阈值右移位数",7,0,12,1),

        t("边缘","edge_comp_enabled","边缘补偿开关","增强边缘信号并外推坐标",1),
        s("边缘","edge_boost_pct","边缘信号增强 (%)","边缘像素增强百分比",50,0,200,5),
        s("边缘","edge_push_q8","边缘外推量 (Q8.8)","质心向外最大推移",128,0,384,8),
        s("边缘","edge_blend_q8","边缘混合范围 (Q8.8)","坐标混合过渡宽度",512,64,1024,16),
        t("边缘","edge_reject_enabled","边缘拒绝","拒绝弱边缘触点",1),
        s("边缘","edge_reject_margin","边缘拒绝边距","边缘区域内缩边距",24,0,64,1),
        s("边缘","edge_reject_min_signal","边缘拒绝最小信号","边缘触点保留门槛",500,0,2000,50),

        t("追踪","track_smoothing","轨迹平滑","基于速度预测平滑坐标",1),
        t("追踪","track_active_guard","活动保护","首次确认前清除游离轨迹",1),
        s("追踪","track_lost_frames","允许丢失帧数","轨迹释放前的 silent gap",4,1,10,1),
        s("追踪","debounce_base","防抖基准帧","新触点上报前稳定帧数",1,0,8,1),
        s("追踪","track_start_debounce","起始防抖帧","touch_active 确认帧数",1,0,8,1),
        e("追踪","track_dist2_max","最大匹配距离²","帧间轨迹最大匹配距离平方",176400),
        e("追踪","track_jump_dist2","跳点检测距离²","0 为禁用",0),
        t("追踪","hungarian_enabled","匈牙利匹配","全局最优轨迹匹配",1),
        s("追踪","debounce_weak_extra","弱信号附加防抖","弱信号额外稳定帧",1,0,8,1),
        s("追踪","debounce_edge_extra","边缘附加防抖","边缘触点额外稳定帧",1,0,8,1),
        s("追踪","debounce_strong_signal","强信号阈值","强触点减少防抖",3000,0,8000,100),
        t("追踪","ghost_enabled","鬼点过滤","过滤双指对向滑动鬼点",1),
        s("追踪","ghost_row_distance","鬼点行间距","允许的最大行距离",32,0,64,1),
        s("追踪","ghost_weak_ratio_q8","鬼点弱信号比率 (Q8)","弱信号比率阈值",96,0,255,1),
        s("追踪","ghost_min_col_distance","鬼点最小列间距","列方向最小间距",300,0,800,10),
        t("追踪","euro_enabled","速度自适应平滑","1-Euro 坐标平滑",1),
        s("追踪","euro_alpha_min_q8","Euro 速率下限 (Q8)","低速平滑系数",64,1,255,1),
        s("追踪","euro_alpha_max_q8","Euro 速率上限 (Q8)","高速平滑系数",224,1,255,1),
        s("追踪","euro_speed_threshold","Euro 速度阈值","达到最大系数的速度",24,1,64,1),
        s("追踪","gesture_drag_distance","拖拽手势距离","判定拖拽最小距离",80,0,256,4),
        s("追踪","gesture_long_press_frames","长按手势帧数","判定长按持续帧数",45,1,120,1)
    );

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        EdgeToEdge.enable(this);
        LinearLayout root = new LinearLayout(this); root.setOrientation(LinearLayout.VERTICAL);
        toolbar = new MaterialToolbar(this); toolbar.setTitle("EGoTouch 调参器");
        root.addView(toolbar, new LinearLayout.LayoutParams(-1, -2));
        content = new FrameLayout(this); root.addView(content, new LinearLayout.LayoutParams(-1, 0, 1));
        BottomNavigationView nav = new BottomNavigationView(this); nav.inflateMenu(R.menu.main_navigation);
        nav.setOnItemSelectedListener(item -> { if(item.getItemId()==R.id.nav_diagnostic) showDiagnostic(); else showTuner(); return true; });
        root.addView(nav, new LinearLayout.LayoutParams(-1, -2)); setContentView(root);
        installEdgeToEdgeInsets();
        showTuner(); discoverAndLoad();
    }

    private void installEdgeToEdgeInsets() {
        final int toolbarLeft = toolbar.getPaddingLeft();
        final int toolbarTop = toolbar.getPaddingTop();
        final int toolbarRight = toolbar.getPaddingRight();
        final int toolbarBottom = toolbar.getPaddingBottom();
        ViewCompat.setOnApplyWindowInsetsListener(toolbar, (view, windowInsets) -> {
            Insets safe = windowInsets.getInsets(
                    WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout());
            view.setPadding(toolbarLeft + safe.left, toolbarTop + safe.top,
                    toolbarRight + safe.right, toolbarBottom);
            return windowInsets;
        });
        ViewCompat.setOnApplyWindowInsetsListener(content, (view, windowInsets) -> {
            Insets safe = windowInsets.getInsets(
                    WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout());
            view.setPadding(safe.left, 0, safe.right, 0);
            return windowInsets;
        });
        ViewCompat.requestApplyInsets(toolbar);
        ViewCompat.requestApplyInsets(content);
    }

    private void showTuner() {
        tunerVisible = true;
        toolbar.setTitle("EGoTouch 调参器");
        LinearLayout page = vertical(); page.setPadding(dp(16),dp(8),dp(16),dp(16));
        MaterialCardView statusCard = new MaterialCardView(this);
        statusCard.setCardElevation(0); statusCard.setStrokeWidth(dp(1)); statusCard.setUseCompatPadding(true);
        LinearLayout statusBox = vertical(); statusBox.setPadding(dp(16),dp(10),dp(16),dp(10)); statusCard.addView(statusBox);
        TextView state = body(connectionMessage); statusBox.addView(state);
        if (connectionFailed) {
            MaterialButton retry = new MaterialButton(this, null,
                    com.google.android.material.R.attr.materialButtonOutlinedStyle);
            retry.setText("重新连接"); retry.setIconResource(R.drawable.ic_restart_alt);
            retry.setOnClickListener(v -> discoverAndLoad()); statusBox.addView(retry);
        }
        page.addView(statusCard);
        String detectedMode = detectCurrentMode();
        LinearLayout modeHeader = new LinearLayout(this); modeHeader.setGravity(Gravity.CENTER_VERTICAL);
        TextView modeLabel = body("当前配置");
        Chip modeStatus = new Chip(this); modeStatus.setCheckable(false); modeStatus.setClickable(false);
        modeStatus.setText(connectionBusy ? "读取中" : connectionFailed ? "不可用" :
                "game".equals(detectedMode) ? "游戏模式" : "daily".equals(detectedMode) ? "日用模式" : "自定义");
        modeHeader.addView(modeLabel, new LinearLayout.LayoutParams(0,-2,1)); modeHeader.addView(modeStatus);
        page.addView(modeHeader);
        TextView immediateHint = body("选择预设或调整参数后会立即写入当前内核；无需另行保存。");
        immediateHint.setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodySmall);
        page.addView(immediateHint);
        ChipGroup modes = new ChipGroup(this); modes.setSingleSelection(true); modes.setSelectionRequired(false);
        Chip daily=chip("应用日用预设"), game=chip("应用游戏预设");
        daily.setId(View.generateViewId()); game.setId(View.generateViewId());
        modes.addView(daily); modes.addView(game);
        if ("game".equals(detectedMode)) game.setChecked(true);
        else if ("daily".equals(detectedMode)) daily.setChecked(true);
        daily.setEnabled(!connectionBusy && !connectionFailed && presetSupported());
        game.setEnabled(!connectionBusy && !connectionFailed && presetSupported());
        page.addView(modes);
        daily.setOnClickListener(v -> applyPreset(false)); game.setOnClickListener(v -> applyPreset(true));
        TabLayout tabs = new TabLayout(this); for(String g:new String[]{"预处理","检测","边缘","追踪"}) tabs.addTab(tabs.newTab().setText(g)); page.addView(tabs);
        FrameLayout listHost = new FrameLayout(this); page.addView(listHost,new LinearLayout.LayoutParams(-1,0,1));
        tabs.addOnTabSelectedListener(new TabLayout.OnTabSelectedListener(){ public void onTabSelected(TabLayout.Tab tab){ buildParamGroup(listHost,String.valueOf(tab.getText())); } public void onTabUnselected(TabLayout.Tab t){} public void onTabReselected(TabLayout.Tab t){} });
        buildParamGroup(listHost,"预处理"); content.removeAllViews(); content.addView(page);
    }

    private void discoverAndLoad() {
        final int generation = operationGeneration.incrementAndGet();
        connectionBusy = true; connectionFailed = false; algoPath = null;
        values = Collections.emptyMap(); diagnosticChecked = false;
        diagnosticSupported = false; resetSupported = false;
        diagnosticMessage = "正在检查当前内核的诊断能力…";
        connectionMessage = "正在请求 root 权限…";
        refreshVisiblePage();
        worker.execute(() -> { try {
            String uid = RootShell.run("id -u");
            if (!isCurrent(generation)) return;
            if (!"0".equals(uid.trim())) throw new IllegalStateException("su 未返回 root 身份（uid=" + uid + "）");
            postIfCurrent(generation, () -> { connectionMessage = "Root 已授权，正在读取内核能力…"; refreshVisiblePage(); });
            String foundPath=RootShell.run("P='/sys/devices/platform/soc@0/9c0000.geniqup/998000.spi/spi_master/spi0/spi0.0/algo'; if [ -d \"$P\" ]; then echo \"$P\"; else find /sys/devices -name peak_threshold -type f 2>/dev/null | head -1 | xargs -r dirname; fi");
            Map<String,Integer> snapshot = new HashMap<>();
            if (!foundPath.isEmpty()) {
                String out=RootShell.run("for p in "+RootShell.quote(foundPath)+"/*; do [ -f \"$p\" ] && printf '%s=%s\\n' \"${p##*/}\" \"$(cat \"$p\" 2>/dev/null)\"; done");
                for(String line:out.split("\\n")){int p=line.indexOf('='); if(p>0) try{snapshot.put(line.substring(0,p),Integer.parseInt(line.substring(p+1).trim()));}catch(Exception ignored){}}
            }
            String capabilities = RootShell.run(
                    "for n in diagnostics event_stack event_trace; do p=$(find /sys/devices -name $n -type f 2>/dev/null|head -1); [ -n \"$p\" ] && echo $n=1 || echo $n=0; done; " +
                    "p=$(find /sys/devices -name inplace_reset -type f 2>/dev/null|head -1); [ -n \"$p\" ] && echo inplace_reset=1 || echo inplace_reset=0; " +
                    "ok=0; for n in /sys/class/input/event*/device/name; do [ -f \"$n\" ] || continue; case $(cat \"$n\" 2>/dev/null) in *Himax*Touch*) ok=1; break;; esac; done; echo himax_input=$ok");
            if (!isCurrent(generation)) return;
            boolean hasDiagnostics=capabilities.contains("diagnostics=1");
            boolean hasStack=capabilities.contains("event_stack=1");
            boolean hasTrace=capabilities.contains("event_trace=1");
            boolean hasInput=capabilities.contains("himax_input=1");
            boolean hasReset=capabilities.contains("inplace_reset=1");
            List<String> missing = new ArrayList<>();
            if(!hasDiagnostics)missing.add("diagnostics"); if(!hasStack)missing.add("event_stack");
            if(!hasTrace)missing.add("event_trace"); if(!hasInput)missing.add("Himax input");
            Map<String,Integer> published=Collections.unmodifiableMap(snapshot);
            postIfCurrent(generation, () -> {
                algoPath=foundPath.isEmpty()?null:foundPath; values=published;
                connectionBusy=false; connectionFailed=foundPath.isEmpty();
                connectionMessage=foundPath.isEmpty()?"Root 已授权，但未找到 Himax 驱动调参节点":"已获得 root 权限 · 已连接驱动\n"+foundPath;
                diagnosticChecked=true; diagnosticSupported=missing.isEmpty(); resetSupported=hasReset;
                diagnosticMessage=missing.isEmpty()?"当前内核支持完整断触诊断":"当前内核不支持完整诊断，缺少："+String.join("、",missing);
                refreshVisiblePage();
            });
        } catch(Exception ex){
            String detail = ex.getMessage() == null ? ex.toString() : ex.getMessage();
            postIfCurrent(generation, () -> {
                connectionBusy=false; connectionFailed=true; algoPath=null; values=Collections.emptyMap();
                diagnosticChecked=true; diagnosticSupported=false; resetSupported=false;
                diagnosticMessage="无法检查诊断能力：root 不可用";
                if (detail.contains("Cannot run program \"su\"") || detail.contains("Permission denied")) connectionMessage="Root 授权失败\n请在 KernelSU/Magisk 中允许 EGoTouch 调参器获得超级用户权限。";
                else connectionMessage="连接失败\n"+detail;
                refreshVisiblePage();
            });
        }});
    }

    private boolean isCurrent(int generation){return !destroyed&&operationGeneration.get()==generation;}
    private void postIfCurrent(int generation,Runnable action){if(!isCurrent(generation))return;runOnUiThread(()->{if(isCurrent(generation))action.run();});}
    private void refreshVisiblePage(){if(destroyed||content==null)return;if(tunerVisible)showTuner();else showDiagnostic();}

    private String detectCurrentMode() {
        if (connectionBusy || connectionFailed || values.isEmpty()) return "custom";
        String[] keys = {"track_smoothing","track_start_debounce","track_jump_dist2",
                "euro_enabled","debounce_base","debounce_weak_extra","debounce_edge_extra",
                "hungarian_enabled","ghost_enabled"};
        int[] game = {0,0,6400,0,0,0,0,1,1};
        boolean gameMatch = true, dailyMatch = true;
        for (int i=0;i<keys.length;i++) {
            Integer current = values.get(keys[i]);
            if (current == null || current != game[i]) gameMatch = false;
            int defaultValue = 0;
            for (Param p : PARAMS) if (p.key.equals(keys[i])) { defaultValue = p.defaultValue; break; }
            if (current == null || current != defaultValue) dailyMatch = false;
        }
        return gameMatch ? "game" : dailyMatch ? "daily" : "custom";
    }

    private boolean presetSupported(){for(String key:new String[]{"track_smoothing","track_start_debounce","track_jump_dist2","euro_enabled","debounce_base","debounce_weak_extra","debounce_edge_extra","hungarian_enabled","ghost_enabled"})if(!values.containsKey(key))return false;return true;}

    private void buildParamGroup(FrameLayout host,String group){
        ScrollView scroll=new ScrollView(this); LinearLayout list=vertical(); list.setPadding(0,dp(8),0,dp(24)); scroll.addView(list);
        for(Param p:PARAMS) if(p.group.equals(group)) list.addView(paramCard(p)); host.removeAllViews(); host.addView(scroll);
    }

    private View paramCard(Param p){
        MaterialCardView card=new MaterialCardView(this); card.setUseCompatPadding(true); card.setCardElevation(0); card.setStrokeWidth(dp(1));
        LinearLayout box=vertical(); box.setPadding(dp(16),dp(12),dp(16),dp(12)); card.addView(box);
        boolean supported=values.containsKey(p.key); boolean enabled=supported&&!connectionBusy&&!connectionFailed;
        int value=supported?values.get(p.key):p.defaultValue;
        if(p.toggle){ SwitchMaterial sw=new SwitchMaterial(this); sw.setText(p.title); sw.setChecked(value!=0); sw.setEnabled(enabled); sw.setOnCheckedChangeListener((b,on)->write(p,on?1:0)); box.addView(sw); }
        else if(p.entry){ TextInputLayout til=new TextInputLayout(this); til.setHint(p.title); til.setEndIconMode(TextInputLayout.END_ICON_NONE); til.setEnabled(enabled); TextInputEditText edit=new TextInputEditText(til.getContext()); edit.setInputType(InputType.TYPE_CLASS_NUMBER|InputType.TYPE_NUMBER_FLAG_SIGNED); edit.setText(supported?String.valueOf(value):""); edit.setSingleLine(); edit.setOnEditorActionListener((v,a,e)->{commitEntry(p,edit);return true;}); edit.setOnFocusChangeListener((v,has)->{if(!has)commitEntry(p,edit);}); til.addView(edit); box.addView(til); }
        else { TextView label=body(p.title+"   "+(supported?value:"不支持")); box.addView(label); Slider slider=new Slider(this); slider.setValueFrom(p.min); slider.setValueTo(p.max); slider.setStepSize(p.step); slider.setValue(Math.max(p.min,Math.min(p.max,value))); slider.setEnabled(enabled); slider.addOnChangeListener((s,v,user)->label.setText(p.title+"   "+Math.round(v))); slider.addOnSliderTouchListener(new Slider.OnSliderTouchListener(){public void onStartTrackingTouch(Slider s){} public void onStopTrackingTouch(Slider s){write(p,Math.round(s.getValue()));}}); box.addView(slider); }
        TextView desc=body(supported?p.description:p.description+" · 当前内核不支持此参数"); desc.setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodySmall); desc.setEnabled(supported); box.addView(desc); return card;
    }

    private void commitEntry(Param p,TextInputEditText edit){try{write(p,Integer.parseInt(String.valueOf(edit.getText())));}catch(Exception ex){edit.setError("请输入整数");}}
    private void write(Param p,int value){if(algoPath==null||connectionBusy||!values.containsKey(p.key)){toast("该参数不可用");return;}final int generation=operationGeneration.get();final String path=algoPath;worker.execute(()->{try{RootShell.run("printf '%s' "+RootShell.quote(String.valueOf(value))+" > "+RootShell.quote(path+"/"+p.key));if(!isCurrent(generation))return;Map<String,Integer> updated=new HashMap<>(values);updated.put(p.key,value);Map<String,Integer> published=Collections.unmodifiableMap(updated);postIfCurrent(generation,()->{values=published;refreshVisiblePage();toast(p.title+" 已生效");});}catch(Exception ex){postIfCurrent(generation,()->toast("写入 "+p.title+" 失败："+ex.getMessage()));}});}
    private void applyPreset(boolean game){
        if (algoPath == null || connectionBusy || connectionFailed || !presetSupported()) { toast("当前内核不支持完整模式预设"); return; }
        Map<String,Integer> preset=new HashMap<>();
        for(String k:new String[]{"track_smoothing","track_start_debounce","track_jump_dist2","euro_enabled","debounce_base","debounce_weak_extra","debounce_edge_extra","hungarian_enabled","ghost_enabled"})
            for(Param p:PARAMS) if(p.key.equals(k)) preset.put(k,p.defaultValue);
        if(game){preset.put("track_smoothing",0);preset.put("track_start_debounce",0);preset.put("track_jump_dist2",6400);preset.put("euro_enabled",0);preset.put("debounce_base",0);preset.put("debounce_weak_extra",0);preset.put("debounce_edge_extra",0);}
        final int generation=operationGeneration.incrementAndGet(); final String path=algoPath;
        connectionBusy = true; connectionMessage = "正在应用" + (game ? "游戏" : "日用") + "模式…"; showTuner();
        worker.execute(()->{try{
            StringBuilder cmd=new StringBuilder();
            for(Map.Entry<String,Integer>x:preset.entrySet()) cmd.append("printf '%s' ").append(RootShell.quote(String.valueOf(x.getValue()))).append(" > ").append(RootShell.quote(path+"/"+x.getKey())).append(" || exit; ");
            RootShell.run(cmd.toString());
            if(!isCurrent(generation))return;
            StringBuilder readback = new StringBuilder();
            for(String key:preset.keySet()) readback.append("printf '").append(key).append("=%s\\n' \"$(cat ").append(RootShell.quote(path+"/"+key)).append(")\"; ");
            String output = RootShell.run(readback.toString());
            if(!isCurrent(generation))return;Map<String,Integer> updated=new HashMap<>(values);for(String line:output.split("\\n")){int p=line.indexOf('=');if(p>0)updated.put(line.substring(0,p),Integer.parseInt(line.substring(p+1).trim()));}Map<String,Integer> published=Collections.unmodifiableMap(updated);
            postIfCurrent(generation,()->{values=published;connectionBusy=false;connectionMessage="已获得 root 权限 · 已连接驱动 · "+(game?"游戏模式":"日用模式");refreshVisiblePage();toast(game?"游戏模式已应用并验证":"日用模式已应用并验证");});
        }catch(Exception ex){postIfCurrent(generation,()->{connectionBusy=false;connectionFailed=false;connectionMessage="模式切换失败，驱动仍保持连接\n"+ex.getMessage();refreshVisiblePage();});}});
    }

    private void showDiagnostic(){
        tunerVisible=false;
        toolbar.setTitle("Himax 断触诊断"); ScrollView scroll=new ScrollView(this); LinearLayout page=vertical(); page.setPadding(dp(16),dp(16),dp(16),dp(32)); scroll.addView(page);
        TextView hint=body("断触发生后保持现场，点“抓取证据”，随后继续滑动 5 秒。将保存故障前追踪、观察窗口、input 事件、驱动状态和系统日志。数据保存在 /sdcard/HimaxTouchDiag，需要 root。"); page.addView(hint);
        MaterialCardView capabilityCard=new MaterialCardView(this);capabilityCard.setCardElevation(0);capabilityCard.setStrokeWidth(dp(1));capabilityCard.setUseCompatPadding(true);TextView capability=body(diagnosticMessage);capability.setPadding(dp(16),dp(12),dp(16),dp(12));capabilityCard.addView(capability);page.addView(capabilityCard);
        MaterialButton collect=button("抓取证据",R.drawable.ic_bug_report);collect.setEnabled(diagnosticChecked&&diagnosticSupported);collect.setOnClickListener(v->capture(false));page.addView(collect);
        MaterialButton reset=button("硬件重置并记录",R.drawable.ic_restart_alt);reset.setEnabled(diagnosticChecked&&diagnosticSupported&&resetSupported);reset.setOnClickListener(v->new MaterialAlertDialogBuilder(this).setTitle("确认硬件重置").setMessage("请先完成普通采集。此操作会写入驱动 inplace_reset 节点。").setNegativeButton("取消",null).setPositiveButton("重置并记录",(d,w)->capture(true)).show()); page.addView(reset);
        if(diagnosticChecked&&!resetSupported){TextView resetHint=body("当前内核没有 inplace_reset 节点，硬件重置功能不可用。");resetHint.setEnabled(false);page.addView(resetHint);}
        MaterialButton copy=button("复制最近目录",R.drawable.ic_content_copy); copy.setOnClickListener(v->{((ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("Himax 诊断目录",lastCaptureDir));toast("已复制 "+lastCaptureDir);});page.addView(copy);
        content.removeAllViews();content.addView(scroll);
    }

    private void capture(boolean reset){
        if(!diagnosticSupported||(reset&&!resetSupported)){toast("当前内核不支持此诊断操作");return;}final int generation=operationGeneration.incrementAndGet();toast("正在采集 5 秒窗口…"); String stamp=new SimpleDateFormat("yyyyMMdd-HHmmss",Locale.US).format(new Date()); lastCaptureDir=Environment.getExternalStorageDirectory().getPath()+"/HimaxTouchDiag/"+stamp;String captureDir=lastCaptureDir;
        worker.execute(()->{try{String out=RootShell.run(DiagnosticScript.make(captureDir,stamp,reset));postIfCurrent(generation,()->new MaterialAlertDialogBuilder(this).setTitle(out.contains("CAPTURE_STATUS=SUCCESS")?"采集完成":"采集不完整").setMessage(captureDir+"\n\n"+out).setPositiveButton("完成",null).show());}catch(Exception ex){postIfCurrent(generation,()->new MaterialAlertDialogBuilder(this).setTitle("采集失败").setMessage(ex.getMessage()).setPositiveButton("确定",null).show());}});
    }

    private LinearLayout vertical(){LinearLayout x=new LinearLayout(this);x.setOrientation(LinearLayout.VERTICAL);return x;}
    private TextView body(String text){TextView v=new TextView(this);v.setText(text);v.setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodyMedium);v.setPadding(0,dp(6),0,dp(6));return v;}
    private Chip chip(String text){Chip c=new Chip(this);c.setText(text);c.setCheckable(true);return c;}
    private MaterialButton button(String text,int icon){MaterialButton b=new MaterialButton(this);b.setText(text);b.setIconResource(icon);LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2);lp.topMargin=dp(12);b.setLayoutParams(lp);return b;}
    private void toast(String text){Snackbar.make(content,text,Snackbar.LENGTH_LONG).show();}
    private int dp(int value){return Math.round(value*getResources().getDisplayMetrics().density);}
    @Override protected void onDestroy(){destroyed=true;operationGeneration.incrementAndGet();worker.shutdownNow();super.onDestroy();}
}
