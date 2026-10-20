.class final Lcom/trendy/ddapp/Gamepad;
.super Ljava/lang/Object;
.source "Gamepad.java"


# static fields
.field private static active:Z

.field private static final down:[Z

.field private static swallowing:Z


# direct methods
.method static constructor <clinit>()V
    .locals 1

    .line 23
    const/16 v0, 0x100

    new-array v0, v0, [Z

    sput-object v0, Lcom/trendy/ddapp/Gamepad;->down:[Z

    return-void
.end method

.method constructor <init>()V
    .locals 0

    .line 21
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V

    return-void
.end method

.method private static fromController(I)Z
    .locals 2

    .line 81
    and-int/lit16 v0, p0, 0x401

    const/16 v1, 0x401

    if-eq v0, v1, :cond_1

    const v0, 0x1000010

    and-int/2addr p0, v0

    if-ne p0, v0, :cond_0

    goto :goto_0

    :cond_0
    const/4 p0, 0x0

    goto :goto_1

    :cond_1
    :goto_0
    const/4 p0, 0x1

    :goto_1
    return p0
.end method

.method private static hat(Lcom/trendy/ddapp/ddapp;FII)V
    .locals 3

    .line 91
    const/high16 v0, -0x41000000    # -0.5f

    cmpg-float v0, p1, v0

    const/4 v1, 0x1

    const/4 v2, 0x0

    if-gez v0, :cond_0

    move v0, v1

    goto :goto_0

    :cond_0
    move v0, v2

    :goto_0
    invoke-static {p0, p2, v0}, Lcom/trendy/ddapp/Gamepad;->key(Lcom/trendy/ddapp/ddapp;IZ)V

    .line 92
    const/high16 p2, 0x3f000000    # 0.5f

    cmpl-float p1, p1, p2

    if-lez p1, :cond_1

    goto :goto_1

    :cond_1
    move v1, v2

    :goto_1
    invoke-static {p0, p3, v1}, Lcom/trendy/ddapp/Gamepad;->key(Lcom/trendy/ddapp/ddapp;IZ)V

    .line 93
    return-void
.end method

.method private static key(Lcom/trendy/ddapp/ddapp;IZ)V
    .locals 1

    .line 96
    if-ltz p1, :cond_2

    sget-object v0, Lcom/trendy/ddapp/Gamepad;->down:[Z

    array-length v0, v0

    if-ge p1, v0, :cond_2

    sget-object v0, Lcom/trendy/ddapp/Gamepad;->down:[Z

    aget-boolean v0, v0, p1

    if-ne v0, p2, :cond_0

    goto :goto_0

    .line 97
    :cond_0
    sget-object v0, Lcom/trendy/ddapp/Gamepad;->down:[Z

    aput-boolean p2, v0, p1

    .line 98
    if-eqz p2, :cond_1

    const/4 v0, 0x1

    invoke-static {p0, v0}, Lcom/trendy/ddapp/Gamepad;->setActive(Lcom/trendy/ddapp/ddapp;Z)V

    .line 99
    :cond_1
    invoke-virtual {p0, p1, p2}, Lcom/trendy/ddapp/ddapp;->ddportPadKey(IZ)V

    .line 100
    return-void

    .line 96
    :cond_2
    :goto_0
    return-void
.end method

.method static onKey(Lcom/trendy/ddapp/ddapp;Landroid/view/KeyEvent;)Z
    .locals 5

    .line 29
    invoke-virtual {p1}, Landroid/view/KeyEvent;->getKeyCode()I

    move-result v0

    .line 30
    const/16 v1, 0x13

    const/4 v2, 0x0

    const/4 v3, 0x1

    if-lt v0, v1, :cond_0

    const/16 v1, 0x16

    if-gt v0, v1, :cond_0

    move v1, v3

    goto :goto_0

    :cond_0
    move v1, v2

    .line 31
    :goto_0
    const/16 v4, 0x6e

    if-eq v0, v4, :cond_6

    .line 32
    invoke-static {v0}, Landroid/view/KeyEvent;->isGamepadButton(I)Z

    move-result v4

    if-nez v4, :cond_2

    if-eqz v1, :cond_1

    invoke-virtual {p1}, Landroid/view/KeyEvent;->getSource()I

    move-result v1

    invoke-static {v1}, Lcom/trendy/ddapp/Gamepad;->fromController(I)Z

    move-result v1

    if-nez v1, :cond_2

    :cond_1
    goto :goto_1

    .line 35
    :cond_2
    invoke-virtual {p1}, Landroid/view/KeyEvent;->getAction()I

    move-result p1

    .line 36
    if-eqz p1, :cond_3

    if-ne p1, v3, :cond_5

    .line 37
    :cond_3
    if-nez p1, :cond_4

    move v2, v3

    :cond_4
    invoke-static {p0, v0, v2}, Lcom/trendy/ddapp/Gamepad;->key(Lcom/trendy/ddapp/ddapp;IZ)V

    .line 39
    :cond_5
    return v3

    .line 33
    :cond_6
    :goto_1
    return v2
.end method

.method static onMotion(Lcom/trendy/ddapp/ddapp;Landroid/view/MotionEvent;)Z
    .locals 9

    .line 44
    invoke-virtual {p1}, Landroid/view/MotionEvent;->getSource()I

    move-result v0

    const v1, 0x1000010

    and-int/2addr v0, v1

    const/4 v2, 0x0

    if-ne v0, v1, :cond_2

    .line 45
    invoke-virtual {p1}, Landroid/view/MotionEvent;->getAction()I

    move-result v0

    const/4 v1, 0x2

    if-eq v0, v1, :cond_0

    goto/16 :goto_0

    .line 48
    :cond_0
    invoke-virtual {p1, v2}, Landroid/view/MotionEvent;->getAxisValue(I)F

    move-result v0

    const/4 v1, 0x1

    invoke-virtual {p1, v1}, Landroid/view/MotionEvent;->getAxisValue(I)F

    move-result v3

    .line 49
    const/16 v4, 0xb

    invoke-virtual {p1, v4}, Landroid/view/MotionEvent;->getAxisValue(I)F

    move-result v4

    const/16 v5, 0xe

    invoke-virtual {p1, v5}, Landroid/view/MotionEvent;->getAxisValue(I)F

    move-result v5

    .line 50
    invoke-static {v0}, Ljava/lang/Math;->abs(F)F

    move-result v6

    invoke-static {v3}, Ljava/lang/Math;->abs(F)F

    move-result v7

    invoke-static {v6, v7}, Ljava/lang/Math;->max(FF)F

    move-result v6

    invoke-static {v4}, Ljava/lang/Math;->abs(F)F

    move-result v7

    invoke-static {v5}, Ljava/lang/Math;->abs(F)F

    move-result v8

    invoke-static {v7, v8}, Ljava/lang/Math;->max(FF)F

    move-result v7

    invoke-static {v6, v7}, Ljava/lang/Math;->max(FF)F

    move-result v6

    const/high16 v7, 0x3f000000    # 0.5f

    cmpl-float v6, v6, v7

    if-lez v6, :cond_1

    .line 51
    invoke-static {p0, v1}, Lcom/trendy/ddapp/Gamepad;->setActive(Lcom/trendy/ddapp/ddapp;Z)V

    .line 53
    :cond_1
    invoke-virtual {p0, v2, v0, v3}, Lcom/trendy/ddapp/ddapp;->ddportPadAxis(IFF)V

    .line 54
    invoke-virtual {p0, v1, v4, v5}, Lcom/trendy/ddapp/ddapp;->ddportPadAxis(IFF)V

    .line 55
    nop

    .line 56
    const/16 v0, 0x11

    invoke-virtual {p1, v0}, Landroid/view/MotionEvent;->getAxisValue(I)F

    move-result v0

    const/16 v2, 0x17

    invoke-virtual {p1, v2}, Landroid/view/MotionEvent;->getAxisValue(I)F

    move-result v2

    invoke-static {v0, v2}, Ljava/lang/Math;->max(FF)F

    move-result v0

    .line 55
    const/16 v2, 0x68

    invoke-static {p0, v2, v0}, Lcom/trendy/ddapp/Gamepad;->trigger(Lcom/trendy/ddapp/ddapp;IF)V

    .line 57
    nop

    .line 58
    const/16 v0, 0x12

    invoke-virtual {p1, v0}, Landroid/view/MotionEvent;->getAxisValue(I)F

    move-result v0

    const/16 v2, 0x16

    invoke-virtual {p1, v2}, Landroid/view/MotionEvent;->getAxisValue(I)F

    move-result v3

    invoke-static {v0, v3}, Ljava/lang/Math;->max(FF)F

    move-result v0

    .line 57
    const/16 v3, 0x69

    invoke-static {p0, v3, v0}, Lcom/trendy/ddapp/Gamepad;->trigger(Lcom/trendy/ddapp/ddapp;IF)V

    .line 59
    const/16 v0, 0xf

    invoke-virtual {p1, v0}, Landroid/view/MotionEvent;->getAxisValue(I)F

    move-result v0

    const/16 v3, 0x15

    invoke-static {p0, v0, v3, v2}, Lcom/trendy/ddapp/Gamepad;->hat(Lcom/trendy/ddapp/ddapp;FII)V

    .line 60
    const/16 v0, 0x10

    invoke-virtual {p1, v0}, Landroid/view/MotionEvent;->getAxisValue(I)F

    move-result p1

    const/16 v0, 0x13

    const/16 v2, 0x14

    invoke-static {p0, p1, v0, v2}, Lcom/trendy/ddapp/Gamepad;->hat(Lcom/trendy/ddapp/ddapp;FII)V

    .line 61
    return v1

    .line 46
    :cond_2
    :goto_0
    return v2
.end method

.method static onTouch(Lcom/trendy/ddapp/ddapp;Landroid/view/MotionEvent;)Z
    .locals 3

    .line 70
    invoke-virtual {p1}, Landroid/view/MotionEvent;->getActionMasked()I

    move-result p1

    .line 71
    const/4 v0, 0x0

    const/4 v1, 0x1

    if-nez p1, :cond_0

    sget-boolean v2, Lcom/trendy/ddapp/Gamepad;->active:Z

    if-eqz v2, :cond_0

    .line 72
    invoke-static {p0, v0}, Lcom/trendy/ddapp/Gamepad;->setActive(Lcom/trendy/ddapp/ddapp;Z)V

    .line 73
    sput-boolean v1, Lcom/trendy/ddapp/Gamepad;->swallowing:Z

    .line 75
    :cond_0
    sget-boolean p0, Lcom/trendy/ddapp/Gamepad;->swallowing:Z

    .line 76
    if-eq p1, v1, :cond_1

    const/4 v1, 0x3

    if-ne p1, v1, :cond_2

    :cond_1
    sput-boolean v0, Lcom/trendy/ddapp/Gamepad;->swallowing:Z

    .line 77
    :cond_2
    return p0
.end method

.method private static setActive(Lcom/trendy/ddapp/ddapp;Z)V
    .locals 1

    .line 103
    sget-boolean v0, Lcom/trendy/ddapp/Gamepad;->active:Z

    if-eq v0, p1, :cond_0

    .line 104
    sput-boolean p1, Lcom/trendy/ddapp/Gamepad;->active:Z

    .line 105
    invoke-virtual {p0, p1}, Lcom/trendy/ddapp/ddapp;->KeyPadChange(Z)V

    .line 107
    :cond_0
    return-void
.end method

.method private static trigger(Lcom/trendy/ddapp/ddapp;IF)V
    .locals 1

    .line 86
    const/high16 v0, 0x3f000000    # 0.5f

    cmpl-float v0, p2, v0

    if-lez v0, :cond_0

    const/4 p2, 0x1

    invoke-static {p0, p1, p2}, Lcom/trendy/ddapp/Gamepad;->key(Lcom/trendy/ddapp/ddapp;IZ)V

    goto :goto_0

    .line 87
    :cond_0
    const v0, 0x3e99999a    # 0.3f

    cmpg-float p2, p2, v0

    if-gez p2, :cond_1

    const/4 p2, 0x0

    invoke-static {p0, p1, p2}, Lcom/trendy/ddapp/Gamepad;->key(Lcom/trendy/ddapp/ddapp;IZ)V

    .line 88
    :cond_1
    :goto_0
    return-void
.end method
