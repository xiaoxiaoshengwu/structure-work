/* 平面桁架直接刚度法，单位：力 kN，长度/位移 m，E kN/m^2，A m^2。
 * 假设：小变形、线弹性、理想铰接，杆件仅传递轴力；拉力为正。
 * 输入：TRUSS2D 1 / NODES n / 节点数据 / ELEMENTS m / 单元数据 / END。
 * 节点：编号 x y 固定x 固定y 指定位移x 指定位移y 荷载x 荷载y。
 * 单元：编号 起点编号 终点编号 E A；# 开头的内容为行注释。
 * 编号无需连续；每节点有全局 x、y 两个自由度，内部按读入次序编号。
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

/* Windows 命令行保留完整 Unicode 文件路径；其他平台继续使用标准 C 入口。 */
#ifdef _WIN32
#include <wchar.h>
typedef wchar_t PathChar;
#define PATH_TEXT(text) L##text
#define path_compare wcscmp
#define path_number wcstod
#define path_open _wfopen
#define PROGRAM_ENTRY wmain
#else
typedef char PathChar;
#define PATH_TEXT(text) text
#define path_compare strcmp
#define path_number strtod
#define path_open fopen
#define PROGRAM_ENTRY main
#endif

/* 节点数据结构：id 是文件中的外部编号；x/y 为坐标。
 * fix[0]/fix[1] 分别表示 x/y 方向是否约束，1 约束、0 自由。
 * p[0]/p[1] 是已知支座位移，不是荷载；f[0]/f[1] 是节点外力。
 * 数组方向约定固定为 0=x、1=y，避免位移、荷载方向混淆。 */
typedef struct { int id, fix[2]; double x, y, p[2], f[2]; } Node;
/* 杆件数据：i/j 是节点数组下标，不是外部节点号；read_model 完成转换。
 * E 为弹性模量，A 为截面积，L 为实际杆长，c/s 为方向余弦。
 * 原始编号 id 仅用于识别及输出；改变杆端次序不改变拉正轴力。 */
typedef struct { int id, i, j; double E, A, L, c, s; } Element;
typedef struct { int n, m; Node *node; Element *elem; } Model;
static int input_line = 1;

/* 所有异常都给出明确原因并返回非零退出码，避免输出看似正常的假结果。 */
static _Noreturn void die(const char *message) {
    fprintf(stderr, "ERROR (input line %d): %s\n", input_line, message);
    exit(2);
}
/* 统一采用 calloc：将新内存置零，使矩阵对号累加从零开始。
 * count=0 时仍分配一个对象，允许完全约束模型的空自由方程。
 * 分配失败立即终止，禁止对空指针继续计算。调用者负责 free。 */
static void *allocate(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size);
    if (!p) die("Insufficient memory.");
    return p;
}
/* 逐词读取并跳过 # 注释；只接收 ASCII 关键词，数值用点号作小数点。 */
/* 输入词法扫描：逐字符略过空白和 # 注释，维护报错行号。
 * token 缓冲区由调用者提供，容量必须至少 128 字节。
 * 最多接收 127 字节，再写入字符串终止符；EOF 返回 0。
 * 用 unsigned char 转换后调用 isspace，避免负 char 引发未定义行为。 */
static int token(FILE *f, char *buf) {
    int ch, len = 0;
    for (;;) {
        ch = fgetc(f);
        if (ch == EOF) return 0;
        if (ch == '#') { while (ch != '\n' && ch != EOF) ch = fgetc(f); }
        if (ch == '\n') ++input_line;
        if (ch != EOF && !isspace((unsigned char)ch)) break;
    }
    do {
        if (len >= 127) die("Input token is too long.");
        buf[len++] = (char)ch;
        ch = fgetc(f);
    } while (ch != EOF && !isspace((unsigned char)ch) && ch != '#');
    if (ch == '#') { while (ch != '\n' && ch != EOF) ch = fgetc(f); }
    if (ch == '\n') ++input_line;
    buf[len] = 0;
    return 1;
}
/* 关键词大小写敏感；按既定数据顺序检查，发现错位立即报错。 */
static void key(FILE *f, const char *expected) {
    char b[128];
    if (!token(f, b) || strcmp(b, expected)) die("Missing or unexpected keyword.");
}
/* strtod 接受普通小数和科学计数法；end 检查整词被消费。
 * errno 拒绝数值转换溢出；isfinite 拒绝 NaN 和无穷大。
 * 本程序使用默认 C 数值区域设置，小数点为英文句点。 */
static double number(FILE *f) {
    char b[128], *end; double value;
    if (!token(f, b)) die("Missing numeric value.");
    errno = 0; value = strtod(b, &end);
    if (errno || *end || !isfinite(value)) die("Invalid or non-finite number.");
    return value;
}
/* 整数字段先按 double 解析，再检查 int 范围和无小数部分。
 * 节点数量、杆件数量、编号及约束标志均使用此函数。 */
static int integer(FILE *f) {
    double v = number(f);
    if (v < INT_MIN || v > INT_MAX || floor(v) != v) die("Expected integer.");
    return (int)v;
}
/* 将输入的外部编号映射为零基数组下标；线性检索便于课程理解。
 * 未找到节点表示连接信息错误，不可把编号直接作为数组下标。 */
static int find_node(const Model *a, int id) {
    int i;
    for (i = 0; i < a->n; ++i) if (a->node[i].id == id) return i;
    die("An element references a missing node.");
}
/* 零长度杆及非正材料参数不符合本线弹性桁架模型，必须在组装前排除。 */
static void prepare(Model *a) {
    int e;
    for (e = 0; e < a->m; ++e) {
        Element *b = a->elem + e;
        double dx = a->node[b->j].x - a->node[b->i].x;
        double dy = a->node[b->j].y - a->node[b->i].y;
        b->L = hypot(dx, dy);
        if (!(b->L > 0) || !isfinite(b->L) || b->i == b->j)
            die("Element length must be finite and positive.");
        if (!(b->E > 0) || !(b->A > 0)) die("E and A must be positive.");
        b->c = dx / b->L; b->s = dy / b->L;
        if (!isfinite(b->E * b->A / b->L)) die("Element stiffness overflow.");
    }
}
/* 完整读入流程：版本 -> 节点表 -> 杆件表 -> END -> 几何预处理。
 * 节点坐标、支座条件和荷载保存在同一行；每根杆独立给 E、A。
 * 重复编号、缺失端点、自由方向指定位移、END 后多余词均被拒绝。
 * 仅关键词和数值必须为 ASCII；# 注释可写 UTF-8 中文。 */
static void read_model(FILE *f, Model *a) {
    int i, j, first = fgetc(f); char b[128];
    /* 兼容 Windows 记事本写出的 UTF-8 BOM；其余内容仍用 ASCII 字段。 */
    if (first == 0xEF) {
        if (fgetc(f) != 0xBB || fgetc(f) != 0xBF) die("Invalid UTF-8 BOM.");
    } else if (first != EOF) ungetc(first,f);
    key(f, "TRUSS2D"); if (integer(f) != 1) die("Unsupported format version.");
    key(f, "NODES"); a->n = integer(f);
    if (a->n < 2 || a->n > 1000) die("Node count must be 2..1000.");
    a->node = allocate((size_t)a->n, sizeof(Node));
    for (i = 0; i < a->n; ++i) {
        Node *n = a->node + i;
        n->id = integer(f); n->x = number(f); n->y = number(f);
        for (j = 0; j < i; ++j)
            if (a->node[j].id == n->id) die("Duplicate node ID.");
        n->fix[0] = integer(f); n->fix[1] = integer(f);
        n->p[0] = number(f); n->p[1] = number(f);
        n->f[0] = number(f); n->f[1] = number(f);
        for (j = 0; j < 2; ++j) {
            if (n->fix[j] != 0 && n->fix[j] != 1) die("Fixity must be 0 or 1.");
            if (!n->fix[j] && n->p[j] != 0) die("Free DOF cannot have prescribed displacement.");
        }
    }
    key(f, "ELEMENTS"); a->m = integer(f);
    if (a->m < 1 || a->m > 10000) die("Element count must be 1..10000.");
    a->elem = allocate((size_t)a->m, sizeof(Element));
    for (i = 0; i < a->m; ++i) {
        Element *e = a->elem + i;
        e->id = integer(f); e->i = find_node(a, integer(f));
        e->j = find_node(a, integer(f)); e->E = number(f); e->A = number(f);
        for (j = 0; j < i; ++j)
            if (a->elem[j].id == e->id) die("Duplicate element ID.");
    }
    key(f, "END"); if (token(f, b)) die("Unexpected data after END.");
    if (ferror(f)) die("Input read failed.");
    prepare(a);
}
/* 必做题：底杆 6 长 l，顶点 4 的高度为 h；节点 3 由两侧 30 度确定。
 * 节点 1、2 的两个方向均固定；100 kN 向右下 45 度，另有向右 20 kN。
 * 本次必做题外轮廓为边长1 m的等边三角形，取l=1、h=sqrt(3)/2。 */
static void required_model(Model *a, double l, double h) {
    const int ends[6][2] = {{0,3},{2,3},{1,3},{0,2},{2,1},{0,1}};
    int i;
    if (!(l > 0) || !isfinite(l)) die("l must be finite and positive (m).");
    if (!(h > l/(2*sqrt(3.0))) || !isfinite(h))
        die("h must be finite and above node 3: h > l/(2*sqrt(3)) (m).");
    a->n = 4; a->m = 6;
    a->node = allocate(4, sizeof(Node)); a->elem = allocate(6, sizeof(Element));
    for (i = 0; i < 4; ++i) a->node[i].id = i + 1;
    a->node[1].x = l;
    a->node[2].x = l / 2; a->node[2].y = l / (2 * sqrt(3.0));
    a->node[3].x = l / 2; a->node[3].y = h;
    for (i = 0; i < 2; ++i) a->node[i].fix[0] = a->node[i].fix[1] = 1;
    a->node[3].f[0] = 20 + 100 / sqrt(2.0); a->node[3].f[1] = -100 / sqrt(2.0);
    for (i = 0; i < 6; ++i) {
        Element *e = a->elem + i;
        e->id = i + 1; e->i = ends[i][0]; e->j = ends[i][1];
        e->E = 2e7; e->A = 0.24;
    }
    prepare(a);
}
/* 总控输出采用 17 位有效数字，尽量保证 double 读写往返精度。
 * 输出端点使用外部编号，不能把内部 i/j 下标直接写回文件。
 * 本函数只写文件；输出错误在主函数统一检查。 */
static void write_model(FILE *f, const Model *a, double l, double h) {
    int i;
    fprintf(f, "# Required problem: base length l = %.17g m; top height h = %.17g m.\n", l,h);
    fprintf(f, "# This numerical parameter is supplied by the user, not by the diagram.\n");
    fprintf(f, "TRUSS2D 1\nNODES %d\n# id x y fix_x fix_y u_x u_y F_x F_y\n", a->n);
    for (i = 0; i < a->n; ++i) {
        const Node *n = a->node + i;
        fprintf(f, "%d %.17g %.17g %d %d %.17g %.17g %.17g %.17g\n",
            n->id,n->x,n->y,n->fix[0],n->fix[1],n->p[0],n->p[1],n->f[0],n->f[1]);
    }
    fprintf(f, "ELEMENTS %d\n# id node_i node_j E A\n", a->m);
    for (i = 0; i < a->m; ++i) {
        const Element *e = a->elem + i;
        fprintf(f, "%d %d %d %.17g %.17g\n", e->id,a->node[e->i].id,
            a->node[e->j].id,e->E,e->A);
    }
    fprintf(f, "END\n");
}
/* 对角缩放后的 Cholesky：B = D^-1 Kff D^-1 = LL^T。
 * 只求解自由自由度，约束位移的作用移到右端项；无需罚因子。
 * 负/近零主元意味着机构、约束不足或病态，不能继续计算。 */
/* b 用行优先 n*n 数组保存缩放刚度；下三角原地改写为 L。
 * i/j/k 的三重循环对应 Lij=(Bij-sum(Lik*Ljk))/Ljj。
 * 对角项取平方根前检查 >1e-12；此阈值作用于缩放后的矩阵。
 * 前代求 L*y=缩放荷载；回代求 L^T*x=y，结果原地保存在 x。
 * 上三角旧数据无需更新，回代明确读取下三角的转置位置。
 * n=0 时循环均跳过，支持没有自由未知量的完全约束模型。 */
static void cholesky(double *b, double *x, int n) {
    int i, j, k;
    for (i = 0; i < n; ++i) for (j = 0; j <= i; ++j) {
        double v = b[(size_t)i*n+j];
        for (k = 0; k < j; ++k) v -= b[(size_t)i*n+k]*b[(size_t)j*n+k];
        if (i == j) {
            if (!(v > 1e-12) || !isfinite(v))
                die("Unstable or ill-conditioned structure: stiffness is not positive definite.");
            b[(size_t)i*n+j] = sqrt(v);
        } else b[(size_t)i*n+j] = v / b[(size_t)j*n+j];
    }
    for (i = 0; i < n; ++i) {
        for (j = 0; j < i; ++j) x[i] -= b[(size_t)i*n+j]*x[j];
        x[i] /= b[(size_t)i*n+i];
    }
    for (i = n-1; i >= 0; --i) {
        for (j = i+1; j < n; ++j) x[i] -= b[(size_t)j*n+i]*x[j];
        x[i] /= b[(size_t)i*n+i];
    }
}
/* 求解器主流程及变量：
 * nd=2*n 为完整自由度数；nf 为未约束自由度数。
 * 完整编号 2*i 是节点 i 的 x，2*i+1 是 y（均为零基）。
 * map[q] 将自由方程 q 映射到完整自由度；仅含未约束方向。
 * K 为完整刚度，u 为完整位移，r=K*u-F 为反力/自由残差。
 * B 为缩放缩减刚度，rhs 为缩放右端，scale[q]=sqrt(Kqq)。
 * 原始 K 不被分解覆盖，保留用于反力和残差核对。
 * 当前实现是稠密矩阵：内存约 O(nd^2)，分解约 O(nf^3)。 */
static void solve(const Model *a, FILE *out, double l, double h) {
    int nd = 2*a->n, nf = 0, i, j, e;
    int *map = allocate((size_t)nd, sizeof(int));
    /* K 对应课件总刚数组 r；node.f/u 对应 dp 求解前的荷载/求解后的位移。
     * 输出 N 对应课件杆力数组 h，注意与本程序的顶高参数 h 区别。 */
    double *K = allocate((size_t)nd*nd, sizeof(double));
    double *u = allocate((size_t)nd, sizeof(double));
    double *r = allocate((size_t)nd, sizeof(double));
    double *B, *rhs, *scale, residual = 0, force_scale = 1;
    double energy = 0, work = 0, balance[3] = {0,0,0};
    /* 课件 elcs 几何变换：q=(-c,-s,c,s)，ke=(EA/L)q q^T。
     * 课件 i0、j0 为杆端自由度起址；这里使用完整全局起址 2*i、2*j。
     * 四块分别为 +C、-C、-C、+C；两端均检查约束，固定点无需排在最前。 */
    for (e = 0; e < a->m; ++e) {
        const Element *b = a->elem + e;
        int d[4] = {2*b->i,2*b->i+1,2*b->j,2*b->j+1};
        double q[4] = {-b->c,-b->s,b->c,b->s}, k = b->E*b->A/b->L;
        for (i = 0; i < 4; ++i) for (j = 0; j < 4; ++j)
            K[(size_t)d[i]*nd+d[j]] += k*q[i]*q[j];
    }
    /* 先回填约束位移；自由位移先置零，再建立 map。
     * 节点可任意编号与排序，单向滚动支座也可直接表示。 */
    for (i = 0; i < nd; ++i) {
        const Node *n = a->node + i/2;
        u[i] = n->fix[i%2] ? n->p[i%2] : 0;
        if (!n->fix[i%2]) map[nf++] = i;
    }
    /* 施加边界条件：Kff*uf=Ff-Kfc*uc。
     * u 的自由方向目前为零，所以遍历完整列计算 Kfc*uc。
     * 对角缩放 Dqq=sqrt(Kqq)，B=D^-1*Kff*D^-1；
     * 求解 B*z=D^-1*右端，最后 uf=D^-1*z。 */
    B = allocate((size_t)nf*nf, sizeof(double));
    rhs = allocate((size_t)nf, sizeof(double)); scale = allocate((size_t)nf, sizeof(double));
    for (i = 0; i < nf; ++i) {
        double diag = K[(size_t)map[i]*nd+map[i]];
        if (!(diag > 0) || !isfinite(diag)) die("A free DOF has zero or invalid stiffness.");
        scale[i] = sqrt(diag);
        rhs[i] = a->node[map[i]/2].f[map[i]%2];
        for (j = 0; j < nd; ++j) rhs[i] -= K[(size_t)map[i]*nd+j]*u[j];
        rhs[i] /= scale[i];
    }
    for (i = 0; i < nf; ++i) for (j = 0; j < nf; ++j)
        B[(size_t)i*nf+j] = K[(size_t)map[i]*nd+map[j]] / scale[i] / scale[j];
    cholesky(B, rhs, nf);
    for (i = 0; i < nf; ++i) u[map[i]] = rhs[i]/scale[i];
    /* 用原始 K 回代，约束自由度得到反力，自由自由度得到残差。 */
    for (i = 0; i < nd; ++i) {
        const Node *n = a->node + i/2; double ku = 0;
        if (!isfinite(u[i])) die("Displacement overflow.");
        for (j = 0; j < nd; ++j) ku += K[(size_t)i*nd+j]*u[j];
        r[i] = ku - n->f[i%2]; energy += 0.5*u[i]*ku;
        force_scale = fmax(force_scale, fmax(fabs(ku), fabs(n->f[i%2])));
        if (!n->fix[i%2]) residual = fmax(residual, fabs(r[i]));
        work += u[i] * (n->f[i%2] + (n->fix[i%2] ? r[i] : 0));
    }
    /* 输出使用 12 位小数的科学计数格式，避免微小位移被四舍五入为零。
     * 小模型附原始缩减刚度，便于与手算对照；不输出分解后的矩阵。 */
    fprintf(out, "PLANE TRUSS - DIRECT STIFFNESS METHOD\n");
    fprintf(out, "Units: m, kN, kN/m^2. Axial force: tension +, compression -.\n");
    fprintf(out, "Nodes=%d Elements=%d Free_DOF=%d\n",a->n,a->m,nf);
    if (l > 0) fprintf(out, "Required problem: base l=%.12g m, top h=%.12g m, h/l=%.12g (user parameters).\n",l,h,h/l);
    fprintf(out, "\nFREE DOF MAP\nequation node_id direction\n");
    for (i = 0; i < nf; ++i)
        fprintf(out, "%d %d %c\n",i+1,a->node[map[i]/2].id,map[i]%2 ? 'Y' : 'X');
    /* 教学用小模型打印未三角化的原始 Kff 和修正荷载 Ff-Kfc*uc。 */
    if (nf <= 20) {
        fprintf(out, "\nREDUCED STIFFNESS (kN/m) AND RHS (kN)\n");
        fprintf(out, "equation [Kff row] | corrected_rhs\n");
        for (i = 0; i < nf; ++i) {
            double f = a->node[map[i]/2].f[map[i]%2];
            fprintf(out, "%d",i+1);
            for (j = 0; j < nf; ++j) fprintf(out," %.12e",K[(size_t)map[i]*nd+map[j]]);
            for (j = 0; j < nd; ++j) if (a->node[j/2].fix[j%2])
                f -= K[(size_t)map[i]*nd+j]*a->node[j/2].p[j%2];
            fprintf(out, " | %.12e\n",f);
        }
    } else fprintf(out,"\nKff matrix display omitted for more than 20 free DOFs.\n");
    fprintf(out, "\nELEMENT GEOMETRY\nid L(m) cos(theta) sin(theta) EA/L(kN/m)\n");
    for (e = 0; e < a->m; ++e) {
        const Element *b = a->elem + e;
        fprintf(out,"%d %.12e %.12e %.12e %.12e\n",b->id,b->L,b->c,b->s,b->E*b->A/b->L);
    }
    fprintf(out, "\nNODE RESULTS\nid ux(m) uy(m) Rx(kN) Ry(kN) fix_x fix_y\n");
    for (i = 0; i < a->n; ++i) {
        const Node *n = a->node + i;
        double rx = n->fix[0] ? r[2*i] : 0, ry = n->fix[1] ? r[2*i+1] : 0;
        fprintf(out, "%d %.12e %.12e %.12e %.12e %d %d\n",
            n->id,u[2*i],u[2*i+1],rx,ry,n->fix[0],n->fix[1]);
        balance[0] += rx+n->f[0]; balance[1] += ry+n->f[1];
        balance[2] += n->x*(ry+n->f[1])-n->y*(rx+n->f[0]);
    }
    if (l > 0) {
        fprintf(out, "\nPARAMETER COEFFICIENTS\nid ux/l uy/l (dimensionless)\n");
        for (i = 0; i < a->n; ++i)
            fprintf(out,"%d %.12e %.12e\n",a->node[i].id,u[2*i]/l,u[2*i+1]/l);
    }
    /* 轴向伸长是杆端相对位移沿局部轴线的投影。
     * N=(EA/L)*伸长：伸长为正时拉力为正，缩短时压力为负。
     * 应力 N/A 输出为 kN/m^2；换为 MPa 时应再除以 1000。 */
    fprintf(out, "\nELEMENT RESULTS\nid node_i node_j L(m) N(kN) extension(m) stress(kN/m^2)\n");
    for (e = 0; e < a->m; ++e) {
        const Element *b = a->elem + e;
        double ext = b->c*(u[2*b->j]-u[2*b->i])+b->s*(u[2*b->j+1]-u[2*b->i+1]);
        double N = b->E*b->A/b->L*ext;
        fprintf(out, "%d %d %d %.12e %.12e %.12e %.12e\n",
            b->id,a->node[b->i].id,a->node[b->j].id,b->L,N,ext,N/b->A);
    }
    /* 校核含自由方程残差、整体合力与合矩、能量恒等式。
     * 能量 U=0.5*u^T*K*u；右端 work=u^T*(F+支反力)。
     * work 是最终力与位移乘积，不是线性加载过程的实际外力功。
     * 比较 2*U=work；有支座位移时不可漏掉支反力对应的项。
     * balance 只使用约束方向反力与外力，不将自由残差算作反力。 */
    fprintf(out, "\nCHECKS\nmax_free_residual(kN)=%.12e\nrelative_free_residual=%.12e\n",
        residual,residual/force_scale);
    fprintf(out, "sum_Fx(kN)=%.12e\nsum_Fy(kN)=%.12e\nsum_Mz(kN*m)=%.12e\n",
        balance[0],balance[1],balance[2]);
    fprintf(out, "strain_energy(kN*m)=%.12e\nload_and_support_work(kN*m)=%.12e\n",
        energy,work);
    fprintf(out, "energy_identity_error(kN*m)=%.12e\n",2*energy-work);
    if (!isfinite(energy) || residual/force_scale > 1e-8) die("Numerical verification failed.");
    free(map); free(K); free(u); free(r); free(B); free(rhs); free(scale);
}
/* 命令行入口：
 * 普通模式：truss 输入总控.txt [输出结果.txt]。
 * 参数模式：truss --required l h [输出结果.txt]。
 * 导出模式：truss --write-required l h 输出总控.txt。
 * 无输出路径时结果写 stdout；Windows 使用 wmain 和 _wfopen
 * 保留中文路径，Linux 使用 main 和 fopen。
 * 成功返回 0；用法错误返回 1；输入/求解错误经 die 返回 2。
 * 主函数负责关闭文件并释放模型；异常时由进程退出回收内存。 */
int PROGRAM_ENTRY(int argc, PathChar **argv) {
    Model a = {0}; FILE *in, *out = stdout; double l = 0, h = 0; int write_only = 0;
    const PathChar *output = NULL;
    if (argc >= 2 && (!path_compare(argv[1],PATH_TEXT("--required")) || !path_compare(argv[1],PATH_TEXT("--write-required")))) {
        PathChar *end; write_only = !path_compare(argv[1],PATH_TEXT("--write-required"));
        if (argc < 4 || argc > 5 || (write_only && argc != 5)) die("Incorrect parameter count: l_m and h_m are both required.");
        errno=0; l=path_number(argv[2],&end);
        if (errno || *end) die("Invalid l parameter.");
        errno=0; h=path_number(argv[3],&end); if (errno || *end) die("Invalid h parameter.");
        required_model(&a,l,h); if (argc == 5) output=argv[4];
    } else {
        if (argc < 2 || argc > 3) {
            fprintf(stderr,"Usage: truss.exe input.txt [output.txt]\n"
                "       truss.exe --required l_m h_m [output.txt]\n"
                "       truss.exe --write-required l_m h_m input.txt\n"); return 1;
        }
        in=path_open(argv[1],PATH_TEXT("r")); if (!in) die("Cannot open input file.");
        read_model(in,&a); fclose(in); if (argc == 3) output=argv[2];
    }
    if (output) { out=path_open(output,PATH_TEXT("w")); if (!out) die("Cannot open output file."); }
    if (write_only) write_model(out,&a,l,h); else solve(&a,out,l,h);
    if (ferror(out)) die("Output write failed.");
    if (out != stdout && fclose(out)) die("Output close failed.");
    free(a.node); free(a.elem); return 0;
}
