#ifndef LEDPLAYER7_H
#define LEDPLAYER7_H
#include <string>

typedef void             *LPVOID;
typedef LPVOID				HPROGRAM;	//节目句柄类型
typedef unsigned int       DWORD;
typedef DWORD   COLORREF;
typedef char TCHAR;
typedef bool BOOL;
typedef unsigned int        UINT;
#define FALSE   0
#define TRUE    1
#define LPCTSTR const char *
#define LPTSTR  char *
#define MAX_PATH          260
#ifndef OUT
#define OUT
#endif

#define COLOR_RED			0xff		//红色
#define COLOR_GREEN			0xff00		//绿色
#define COLOR_BLUE			0xff0000	//蓝色
#define COLOR_YELLOW		0xffff		//黄色(红+绿)
#define COLOR_CYAN		  0xffff00		//青色(绿+蓝)
#define COLOR_PURPLE		0Xff00ff		//紫色(红+蓝)
#define COLOR_WHITE		  0xffffff		//白色(红+绿+蓝)

#define ADDTYPE_STRING		0			//添加类型为字串
#define ADDTYPE_FILE		1			//添加的类型为文件


#define OK					0			//函数返回成功

//******节目定时启用日期时间星期的标志宏***************************************************************************
#define ENABLE_DATE			0x01
#define ENABLE_TIME			0x02
#define ENABLE_WEEK			0x04
//*****************************************************************************************************************

//******节目定时星期里某天启用宏***********************************************************
#define WEEK_MON			0x01
#define WEEK_TUES			0x02
#define WEEK_WEN			0x04
#define WEEK_THUR			0x08
#define WEEK_FRI			0x10
#define WEEK_SAT			0x20
#define WEEK_SUN			0x40
//*****************************************************************************

//**通讯设置结构体*********************************************************
typedef struct COMMUNICATIONINFO
{
    int		LEDType;				//LED类型	0.6代T系A系XC系    1.6代E系     2.X1X2		3.C系(C2M,C4M,C2A) 4:E5,E6,C8
    int		SendType;				//通讯方式	0.为Tcp发送（又称固定IP通讯）1:UDP广播 2:串口  3:磁盘保存    4.广域网通讯 5.UDP点对点
    TCHAR	IpStr[16];				//LED屏的IP地址，只有通讯方式为0时才需赋值，其它通讯方式无需赋值
    int		Commport;				//串口号(linux系统该值无效, 串口值通过OutputDir来传递)
    int		Baud;					//波特率，只有通讯方式为2时才需赋值，其它通讯方式无需赋值,   0.9600   1.57600   2.115200  直接赋值 9600，19200，38400，57600，115200亦可
    int		LedNumber;				//LED的屏号，只有通讯方式为2时，且用485通讯时才需赋值. 其它通讯方式无需赋值.
    TCHAR	OutputDir[MAX_PATH];	//通讯方式为2时,为串口路径名,如:"/dev/ttyS0", 通讯方式为3时,磁盘保存的目录,其它通讯方式无需赋值
    TCHAR	NetworkIdStr[19];	//网络ID,只有通讯方式为4时才需赋值，其它通讯方式无需赋值
}*LPCOMMUNICATIONINFO;
//***********************************************************************

//**区域坐标结构体*********************************************************
typedef struct AREARECT
{
    int			left;	//区域左上角横坐标
    int			top;	//区域左上角纵坐标
    int			width;	//区域的宽度
    int			height;	//区域的高度
}*LPAREARECT;
//****************************************************************************

//***字体属性结构对**********************************************************
struct FONTPROP
{
    char		FontPath[MAX_PATH];		//字体路径 字体 DLL 库为字体名(如宋体).SO 库为字体文件的路径（如home/simsun.ttc）
    int			FontSize;			//字号(单位像素)
    COLORREF	FontColor;			//字体颜色
    BOOL		FontBold;			//是否加粗
    BOOL		FontItalic;			//是否斜体
    BOOL		FontUnderLine;		//时否下划线
};
//****************************************************************************

//YYYY年MM月DD日;YY年MM月DD日;DD/MM/YYYY;YYYY/MM/DD;YYYY-MM-DD;YYYY.MM.DD;MM.DD.YYYY;DD.MM.YYYY;
typedef struct DIGITALCLOCKAREAINFO
{
    TCHAR ShowStr[128];				//自定义显示字符串
    FONTPROP ShowStrFont;			//自定义显示字符串字体
    int TimeLagType;				//滞后类型 0为超前，1为滞后
    int HourNum;					//小时数
    int MiniteNum;					//分钟数

    int DateFormat;
    COLORREF DateColor;
    int WeekFormat;
    COLORREF WeekColor;
    int TimeFormat;
    COLORREF TimeColor;

    BOOL IsShowYear;
    BOOL IsShowWeek;
    BOOL IsShowMonth;
    BOOL IsShowDay;
    BOOL IsShowHour;
    BOOL IsShowMinute;
    BOOL IsShowSecond;

    //int HourType;
    //int YearType;
    BOOL IsMutleLineShow;

}*LPDIGITALCLOCKAREAINFO;
//**页面显示的属性结构体****************************************************
struct PLAYPROP
{
    int			InStyle;	//入场特技值（取值范围 0-38） 具体查看文档
    int			OutStyle;	//退场特技值（现无效，预留，置0）
    int			Speed;		//特技显示速度(取值范围1-255)
    int			DelayTime;	//页面留停时间(1-65535)   注：当入场特技为连续左移、连续右移、连续上移、连续下移时，此参数无效
};
//*******************************************************************************

//**计时属性结构体**********************************************************************
typedef struct TIMEAREAINFO
{
    int         ShowFormat;             //显示格式  0.xx天xx时xx分xx秒  1.xx天xx時xx分xx秒  2.xxDayxxHourxxMinxxSec  3.XXdXXhXXmXXs  4.xx:xx:xx:xx
    int         nYear;                  //结束年
    int         nMonth;                 //结束月
    int         nDay;                   //结束日
    int         nHour;                  //结束时
    int         nMinute;                //结束分
    int         nSecond;                //结束秒
    BOOL        IsShowDay;              //是否显示天
    BOOL        IsShowHour;             //是否显示时
    BOOL        IsShowMinute;           //是否显示分
    BOOL        IsShowSecond;           //是否显示秒
    BOOL        IsMutleLineShow;        //是否多行显示，指的是自定义文字与计时文字是否分行显示
    TCHAR       ShowStr[128];           //自定义文字字符串
    COLORREF    TimeStrColor;           //计时文字的颜色
    FONTPROP    ShowFont;               //自定义文字及计时文字颜色，其中FontColor只对文定义文字有效，计时文字颜色为TimeStrColor
}* LPTIMEAREAINFO;

typedef struct VOICEAREAINFO
{
	char        VoiceStr[256];						//文字
	int			DelayTime;						//间隔时间
	int			PlayCount;						//播放速度
}*LPVOICEAREAINFO;

typedef struct QRAREAINFO
{
        int			nLowVersion;//QR的最低版本,0~39
        int			nHighVersion;//QR的最高版本,0~39 一般都固定成39
        int			nErrorCorrectionLevel;//纠错等级0:自适应  1:L 2:M 3:Q
        int			nMaskVersion;//掩膜版本 0xff 自适应  0~7 手动选择
        int         nBlankPointNum;//QR 码周边空白的点数，默认为 4，一般这个值要大于等于 4，当屏幕点数很紧张时，且不够 4 的时候可选择 0，表示区域内周边全做空白处理
        int			nBaseCodeColor;//QR 码底颜色，一般要求是白底，白底识别率会高，也可以设置为其他色，0---红色，1---绿色，2---黄色，3---蓝色，4---紫色（RB 组合），5---青色（GB 组合），6---白色
        BOOL        isColorReverse;//QR 码颜色是否翻转，0---不翻转，1---翻转
        int			nDefaultShow;//默认显示状态，0---黑屏，1---显示默认码
        char     DefaultCode[512];//默认码,字符长度不能超过512)
}*LPQRAREAINFO;


typedef struct PROGRAMTIME
{
            int EnableFlag;		//启用定时的标记，ENABLE_DATE为启用日期,ENABLE_TIME为启用时间,ENABLE_WEEK为启用星期,可用或运算进行组合，如 ENABLE_DATE | ENABLE_TIME | ENABLE_WEEK
            int WeekValue;		//启用星期后，选择要定时的星期里的某些天，用宏 WEEK_MON,WEEK_TUES,WEEK_WEN,WEEK_THUR,WEEK_FRI,WEEK_SAT,WEEK_SUN 通过或运算进行组合
            int StartYear;		//起始年
            int StartMonth;		//起始月
            int StartDay;		//起始日
            int StartHour;		//起姐时
            int StartMinute;	//起始分
            int StartSecond;	//起始秒
            int EndYear;		//结束年
            int EndMonth;		//结束月
            int EndDay;			//结束日
            int EndHour;		//结束时
            int EndMinute;		//结束分
            int EndSecond;		//结束秒
}*LPPROGRAMTIME;

typedef struct WATERBORDERINFO
{
	int			Flag;							//流水边框加载类型标志，0.为动态库预置的边框  1.为从文件加载的边框
	int			BorderType;						//边框的类型，Flag为0是有效，0.单色边框  1.双基色边框  2.全彩边框
	int			BorderValue;					//边框的值，Flag为0是有效，单色边框取值范围是0~39,双基色边框取值范围是0~34,全彩边框取值范围是0~21
	COLORREF	BorderColor;					//边框线颜色,Flag为0并且BorderType为0是才有效
	int			BorderStyle;					//边框显示的样式  0.固定  1.顺时针  2.逆时针  3.闪烁
	int			BorderSpeed;					//边框流动的速度
	TCHAR		WaterBorderBmpPath[MAX_PATH];	//边框图片文件的路径，注意只能是bmp图片，图片大小必需是宽度为32点，取高度小于等于8
}*LPWATERBORDERINFO;


typedef struct LEDCOMMUNICATIONPARAMETER
{
	DWORD	dwMask;				//要修改项的标记  0.修改网络通讯参数  1.修改串口通讯参数  2.修改网口和串口通讯参数
	TCHAR	IpStr[16];			//新的IP地址，只有dwMask为0或2时才需赋值，其它值无需赋值，格式例如 192.168.1.100
	TCHAR	NetMaskStr[16];		//新的子网掩码，只有dwMask为0或2时才需赋值，其它值无需赋值，格式例如 255.255.255.0
	TCHAR	GatewayStr[16];		//新的网关，只有dwMask为0或2时才需赋值，其它值无需赋值,格式例如 192.168.1.1
	TCHAR	MacStr[18];			//新的MAC地址，只有dwMask为0或2时才需赋值，其它值无需赋值，格式例如 12-34-56-78-9a-bc,如无需修改请设为 ff-ff-ff-ff-ff-ff
	int		Baud;				//波特率，只有dwMask为1或2时才需赋值，其它值无需赋值，0.9600  1.57600  2.115200
	int		LedNumber;			//LED屏号 1~255,网络通讯和232通讯赋值 1 即可，485必需和控制卡显示的屏号相同才可通讯
}*LPLEDCOMMUNICATIONPARAMETER;

typedef int (*SERVERINFOCALLBACK)(int Msg, int wParam, void* lParam);
//回调消息枚举
enum
{
	LV_MSG_NONE,
	LV_MSG_CARD_ONLINE,
	LV_MSG_CARD_OFFLINE,
};
//回调参数结构体
typedef struct _card_info
{
	int port;
	char ipStr[16];
	char networkIdStr[19];
}CARD_INFO,*LP_CARD_INFO;

extern "C"
{
    /********************************************************************************************
*   LV_LedInit            初始化缓存文件路径 如"/home/listenvision/"       注意：不调用此接口，缓存文件默认为执行程序的路径
*
*   参数说明
*               workPath        缓存文件路径
********************************************************************************************/
void LV_LedInit(const char *workPath);



/********************************************************************************************
*	LV_InitLed			初始化屏的类型和颜色顺序(C卡)
*  当Led上显示的文字区域的颜色与下发的不一致, 请的确认Led 屏的RGB顺序,并调用此接口
*	参数说明
*				nLedType		屏类型  0.6代T系A系XC系    1.6代E系     2.X1X2		3.7代C系	   4: E5,E6,C8
*				nRgb		模组的RGB顺序,仅C卡有效,其他卡固定为0. C卡时, 0:  R->G->B 1: G->R->B 2:R->B->G 3:B->R->G 4:B->G->R 5:B->B->R
*	返回值   无
*				
********************************************************************************************/
 void LV_InitLed(int nLedType, int nRgb);

/********************************************************************************************
*	LV_CreateProgramEx			创建节目对象，返回类型为 HPROGRAM
*
*	参数说明
*				LedWidth		屏的宽度
*				LedHeight		屏的高度
*				ColorType		屏的颜色 1.单色  2.双基色  3.三基色   注：C卡全彩参数为3      X系列卡参数固定为 4
*				GrayLevel		灰度等级  赋值  1-5对应的灰度等级分别为 无,4,8,16,32   除C卡(C2M C4M,C4A,C2S)外，其它卡传0
*				SaveType		节目保存位置，默认为0保存为flash节目，3保存为ram节目。注：flash节目掉电不清除，ram节目掉电清除。应用场景需要实时刷新的，建议保持为ram节目.仅7代C卡和 E5,E6,C8支持,其他卡默认出货为flash程序,如果需要RAM程序请联系业务或者在官网下载,然后使用Led Player对卡进行升级
*	返回值
*				0				创建节目对象失败
*				非0				创建节目对象成功
********************************************************************************************/
HPROGRAM LV_CreateProgramEx(int LedWidth,int LedHeight,int ColorType,int GrayLevel,int SaveType);
/*********************************************************************************************
*	LV_AddProgram				添加一个节目
*
*	参数说明
*				hProgram		节目对象句柄
*				ProgramNo		节目号，从0开始(0-255)
*				ProgramTime		节目播放时长 0.节目播放时长  非0.指定播放时长
*				LoopCount		循环播放次数
*	返回值
*				0				成功
*				非0				失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_AddProgram(HPROGRAM hProgram,int ProgramNo,int ProgramTime,int LoopCount);

/*********************************************************************************************
*	LV_SetProgramTime			设置节目定时
*	
*	参数说明
*				hProgram		节目对象句柄
*				ProgramNo		节目号 （取值范围0-255)（从0开始）
*				pProgramTime	节目定时属性，设置方式见PROGRAMTIME结构体注示
*	返回值
*				0				成功
*				非0				失败，调用LV_GetError来获取错误信息	
 ********************************************************************************************/
int LV_SetProgramTime(HPROGRAM hProgram,int ProgramNo,LPPROGRAMTIME pProgramTime);

/*********************************************************************************************
*	LV_AddImageTextArea				添加一个图文区域
*
*	参数说明
*				hProgram			节目对象句柄
*				ProgramNo			节目号  从0开始(0-255)
*				AreaNo				区域号	(1-255)
*				pAreaRect			区域坐标属性，设置方式见AREARECT结构体注示
*				IsBackgroundArea	是否为背景区域，1.前景区（默认） 0.背景区  注：除C系列，其它默认为1
*	返回值
*				0					成功
*				非0					失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_AddImageTextArea(HPROGRAM hProgram,int ProgramNo,int AreaNo,LPAREARECT pAreaRect,BOOL IsBackgroundArea);
/*********************************************************************************************
*	LV_AddFileToImageTextArea			添加一个文件到图文区
*
*	参数说明
*				hProgram				节目对象句柄
*				ProgramNo				节目号  从0开始(0-255)
*				AreaNo					区域号	(1-255)
*				FilePath				文件路径，支持的文件类型有 txt  rtf  bmp jpg jpeg 
*				pPlayProp				显示的属性，设置方式见PLAYPROP结构体注示
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_AddFileToImageTextArea(HPROGRAM hProgram,int ProgramNo,int AreaNo,LPCTSTR FilePath,PLAYPROP *pPlayProp);
/*********************************************************************************************
*	LV_AddSingleLineTextToImageTextArea	添加一个单行文本到图文区
*
*	参数说明
*				hProgram				节目对象句柄
*				ProgramNo				节目号  从0开始(0-255)
*				AreaNo					区域号	(1-255)
*				AddType					添加的类型  0.为字符串  1.文件（只支持txt）
*				AddStr					AddType为0则为字符串数据,AddType为1则为文件路径
*				pFontProp				如果AddType为字符串类型或AddType为文件类型且文件为txt则可传入以赋值的该结构体，其它可赋NULL
*				pPlayProp				显示的属性，设置方式见PLAYPROP结构体注示
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_AddSingleLineTextToImageTextArea(HPROGRAM hProgram,int ProgramNo,int AreaNo,int AddType,LPCTSTR AddStr,FONTPROP *pFontProp,PLAYPROP *pPlayProp);

/*********************************************************************************************
*	LV_AddSingleLineTextToImageTextAreaBack	添加一个单行文本到图文区
*
*	参数说明
*				hProgram				节目对象句柄
*				ProgramNo				节目号  从0开始(0-255)
*				AreaNo					区域号	(1-255)
*				AddType					添加的类型  0.为字符串  1.文件（只支持txt）
*				AddStr					AddType为0则为字符串数据,AddType为1则为文件路径
*				pFontProp				如果AddType为字符串类型或AddType为文件类型且文件为txt则可传入以赋值的该结构体，其它可赋NULL
*				pPlayProp				显示的属性，设置方式见PLAYPROP结构体注示
*				BackColor				背景色 0:黑色
*				nRotation				旋转 0:不旋转    1:90度     2:180度     3:270度
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int	LV_AddSingleLineTextToImageTextAreaBack(HPROGRAM hProgram,int ProgramNo,int AreaNo,int AddType,LPCTSTR AddStr,FONTPROP *pFontProp,PLAYPROP *pPlayProp,COLORREF BackColor,int nRotation);




/*********************************************************************************************
*	LV_AddMultiLineTextToImageTextArea	添加一个多行文本到图文区
*
*	参数说明
*				hProgram				节目对象句柄
*				ProgramNo				节目号  从0开始(0-255)
*				AreaNo					区域号  (1-255)
*				AddType					添加的类型  0.为字符串  1.文件（只支持txt）
*				AddStr					AddType为0则为字符串数据,AddType为1则为文件路径    换行符（\n）
*				pFontProp				如果AddType为字符串类型或AddType为文件类型且文件为txt则可传入以赋值的该结构体，其它可赋NULL
*				pPlayProp				显示的属性，设置方式见PLAYPROP结构体注示
*				nAlignment				水平对齐样式，0.左对齐  1.右对齐  2.水平居中  （注意：只对字符串和txt文件有效）
*				IsVCenter				是否垂直居中  0.置顶（默认） 1.垂直居中
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int	LV_AddMultiLineTextToImageTextArea(HPROGRAM hProgram,int ProgramNo,int AreaNo,int AddType,LPCTSTR AddStr,FONTPROP *pFontProp,PLAYPROP *pPlayProp,int nAlignment,BOOL IsVCenter);


/*********************************************************************************************
*	LV_AddMultiLineTextToImageTextAreaBack	添加一个多行文本到图文区
*
*	参数说明
*				hProgram				节目对象句柄
*				ProgramNo				节目号  从0开始(0-255)
*				AreaNo					区域号  (1-255)
*				AddType					添加的类型  0.为字符串  1.文件（只支持txt）
*				AddStr					AddType为0则为字符串数据,AddType为1则为文件路径    换行符（\n）
*				pFontProp				如果AddType为字符串类型或AddType为文件类型且文件为txt则可传入以赋值的该结构体，其它可赋NULL
*				pPlayProp				显示的属性，设置方式见PLAYPROP结构体注示
*				nAlignment				水平对齐样式，0.左对齐  1.右对齐  2.水平居中  （注意：只对字符串和txt文件有效）
*				IsVCenter				是否垂直居中  0.置顶（默认） 1.垂直居中
*				BackColor				背景色 0:黑色
*				nRotation				旋转 0:不旋转    1:90度     2:180度     3:270度
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int	LV_AddMultiLineTextToImageTextAreaBack(HPROGRAM hProgram,int ProgramNo,int AreaNo,int AddType,LPCTSTR AddStr,FONTPROP *pFontProp,PLAYPROP *pPlayProp,int nAlignment,BOOL IsVCenter,int BackColor,int nRotation);


/*********************************************************************************************
 *	LV_AddStaticTextToImageTextArea		添加一个静止文本到图文区
 *	
 *	参数说明
 *				hProgram				节目对象句柄
 *				ProgramNo				节目号  从0开始(0-255)
 *				AreaNo					区域号	(1-255)
 *				AddType					添加的类型  0.为字符串  1.文件（只支持txt）
 *				AddStr					AddType为0则为字符串数据,AddType为1则为文件路径
 *				pFontProp				如果AddType为字符串类型或AddType为文件类型且文件为txt则可传入以赋值的该结构体，其它可赋NULL
 *				DelayTime				显示的时长 1~65535
 *				nAlignment				水平对齐样式，0.左对齐  1.右对齐  2.水平居中  （注意：只对字符串和txt文件有效）
 *				IsVCenter				是否垂直居中  0.置顶（默认） 1.垂直居中
 *	返回值
 *				0						成功
 *				非0						失败，调用LV_GetError来获取错误信息	
 ********************************************************************************************/
int	LV_AddStaticTextToImageTextArea(HPROGRAM hProgram,int ProgramNo,int AreaNo,int AddType,LPCTSTR AddStr,FONTPROP *pFontProp,int DelayTime,int nAlignment,BOOL IsVCenter);

/*********************************************************************************************
 *	LV_AddStaticTextToImageTextAreaBack		添加一个静止文本到图文区
 *	
 *	参数说明
 *				hProgram				节目对象句柄
 *				ProgramNo				节目号  从0开始(0-255)
 *				AreaNo					区域号	(1-255)
 *				AddType					添加的类型  0.为字符串  1.文件（只支持txt）
 *				AddStr					AddType为0则为字符串数据,AddType为1则为文件路径
 *				pFontProp				如果AddType为字符串类型或AddType为文件类型且文件为txt则可传入以赋值的该结构体，其它可赋NULL
 *				DelayTime				显示的时长 1~65535
 *				nAlignment				水平对齐样式，0.左对齐  1.右对齐  2.水平居中  （注意：只对字符串和txt文件有效）
 *				IsVCenter				是否垂直居中  0.置顶（默认） 1.垂直居中
 *				BackColor				背景色 0:黑色
 *				nRotation				旋转 0:不旋转    1:90度     2:180度     3:270度
 *	返回值
 *				0						成功
 *				非0						失败，调用LV_GetError来获取错误信息	
 ********************************************************************************************/
int	LV_AddStaticTextToImageTextAreaBack(HPROGRAM hProgram,int ProgramNo,int AreaNo,int AddType,LPCTSTR AddStr,FONTPROP *pFontProp,int Time,int nAlignment,BOOL IsVCenter,COLORREF BackColor,int nRotation);



/*********************************************************************************************
*	LV_QuickAddSingleLineTextArea		快速添加一个左移单行文本区域
*
*	参数说明
*				hProgram				节目对象句柄
*				ProgramNo				节目号  从0开始(0-255)
*				AreaNo					区域号	(1-255)
*				pAreaRect				区域坐标属性，设置方式见AREARECT结构体注示
*				AddType					添加的类型  0.为字符串  1.文件（只支持txt）
*				AddStr					AddType为0则为字符串数据,AddType为1则为文件路径
*				pFontProp				如果AddType为字符串类型或AddType为文件类型且文件为txt则可传入以赋值的该结构体，其它可赋NULL
*				nSpeed					滚动速度 1~255
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_QuickAddSingleLineTextArea(HPROGRAM hProgram,int ProgramNo,int AreaNo,LPAREARECT pAreaRect,int AddType,LPCTSTR AddStr,FONTPROP *pFontProp,int nSpeed);

/*********************************************************************************************
*	LV_AddDigitalClockArea				添加一个数字时钟区域
*
*	参数说明
*				hProgram				节目对象句柄
*				ProgramNo				节目号 从0开始(0-255)
*				AreaNo					区域号 (1-255)
*				pAreaRect				区域坐标属性，设置方式见AREARECT结构体注示
*				pDigitalClockAreaInfo	数字时钟属性，见DIGITALCLOCKAREAINFO结构体注示
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_AddDigitalClockArea(HPROGRAM hProgram,int ProgramNo,int AreaNo,LPAREARECT pAreaRect,LPDIGITALCLOCKAREAINFO pDigitalClockAreaInfo);

/*********************************************************************************************
 *  LV_AddTimeArea                      添加一个计时区域
 *  
 *  参数说明
 *              hProgram                节目对象句柄
 *              ProgramNo               节目号  从0开始(0-255)
 *              AreaNo                  区域号 (1-255)
 *              pAreaRect               区域坐标属性，设置方式见AREARECT结构体注示
 *              pTimeAreaInfo           计时属性，见TIMEAREAINFO结构体注示
 *  返回值
 *              0                       成功
 *              非0                      失败，调用LV_GetError来获取错误信息 
 ********************************************************************************************/
int LV_AddTimeArea(HPROGRAM hProgram,int ProgramNo,int AreaNo,LPAREARECT pAreaRect,LPTIMEAREAINFO pTimeAreaInfo);

/*********************************************************************************************
 *  LV_AddNeiMaArea                     添加一个内码区域,局部文本刷新，卡型号是否支持和开发流程，请查看内码区域局部更新协议
 *  
 *  参数说明
 *              hProgram                节目对象句柄
 *              ProgramNo               节目号  从0开始(0-255)
 *              AreaNo                  区域号 (1-255)
 *              pAreaRect               区域坐标属性，设置方式见AREARECT结构体注示
 *              NeiMaStr                文本字符串   注：字符串编码是GB2312
 *              FontSize                字体大小 16 24 32
 *              FontColor               文字颜色 格式BBGGRR 0xff 红色  0xff00 绿色  0xffff黄色
 *              pPlayProp               显示的属性，设置方式见PLAYPROP结构体注示
 *  返回值
 *              0                       成功
 *              非0                      失败，调用LV_GetError来获取错误信息 
 ********************************************************************************************/
int LV_AddNeiMaArea(HPROGRAM hProgram,int ProgramNo,int AreaNo,LPAREARECT pAreaRect,LPCTSTR NeiMaStr,int FontSize,int FontColor,PLAYPROP* pPlayProp);

/*********************************************************************************************
*	LV_AddVoiceArea						添加一个语音区域
*	
*	参数说明
*				hProgram				节目对象句柄
*				ProgramNo				节目号 （取值范围0-255)（从0开始）
*				AreaNo					区域号 （取值范围1-255）
*				pVoiceAreaInfo			语音属性，见VOICEAREAINFO结构体注示
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息	
********************************************************************************************/

int LV_AddVoiceArea(HPROGRAM hProgram,int ProgramNo,int AreaNo,LPVOICEAREAINFO pVoiceAreaInfo);

/*********************************************************************************************
*	LV_AddQRArea						添加一个语音区域(目前仅C卡(C2M, C4M)支持)
*	
*	参数说明
*				hProgram				节目对象句柄
*				ProgramNo				节目号 （取值范围0-255)（从0开始）
*				AreaNo					区域号 （取值范围1-255）
*               pAreaRect               区域坐标属性，设置方式见AREARECT结构体注示
*				pQRAreaInfo			   二维码属性，见QRAREAINFO结构体注示
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息	
********************************************************************************************/
int LV_AddQRCodeArea(HPROGRAM hProgram,int ProgramNo,int AreaNo, LPAREARECT pAreaRect,LPQRAREAINFO pQRAreaInfo);
/*********************************************************************************************
*	LV_DeleteProgram					销毁节目对象(注意：如果此节目对象不再使用，请调用此函数销毁，否则会造成内存泄露)
*
*	参数说明
*				hProgram				节目对象句柄
********************************************************************************************/

/*********************************************************************************************
*	LV_AddWaterBorder						添加一个边框
*	
*	参数说明
*				hProgram				节目对象句柄
*				ProgramNo				节目号 （取值范围0-255)（从0开始）
*				AreaNo					区域号 （取值范围1-255）
*				pAreaRect				边框的位置属性
*				pWaterBorderInfo		边框属性，见LPWATERBORDERINFO结构体注示
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息	
********************************************************************************************/
int    LV_AddWaterBorder(HPROGRAM hProgram,int ProgramNo,int AreaNo,LPAREARECT pAreaRect,LPWATERBORDERINFO pWaterBorderInfo);


void LV_DeleteProgram(HPROGRAM hProgram);
/*********************************************************************************************
*	LV_Send								发送节目，此发送为一对一发送
*
*	参数说明
*				pCommunicationInfo		通讯参数，赋值方式见COMMUNICATIONINFO结构体注示
*				hProgram				节目对象句柄
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_Send(LPCOMMUNICATIONINFO pCommunicationInfo,HPROGRAM hProgram);


/*********************************************************************************************
*	LV_RefreshNeiMaArea								刷新内码区域
*	
*	参数说明
*				pCommunicationInfo		通讯参数，赋值方式见COMMUNICATIONINFO结构体注示
*				NeiMaStr				刷新的数据字符串,格式可以查看<<内码区域局部更新协议>>文档
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息	
********************************************************************************************/
int  LV_RefreshNeiMaArea(LPCOMMUNICATIONINFO pCommunicationInfo,LPCTSTR NeiNaStr);

/*********************************************************************************************
*	LV_RefreshVoiceArea								刷新语音播报
*	
*	参数说明
*				pCommunicationInfo		通讯参数，赋值方式见COMMUNICATIONINFO结构体注示
*				VoiceStr				播放的语音字符串
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息	
********************************************************************************************/
int LV_RefreshVoiceArea(LPCOMMUNICATIONINFO pCommunicationInfo, LPCTSTR VoiceStr);

/*********************************************************************************************
*	LV_RefreshQRArea								刷新刷新二维码
*	
*	参数说明
*				pCommunicationInfo		通讯参数，赋值方式见COMMUNICATIONINFO结构体注示
*				CodeStr				    二维码字符串
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息	
********************************************************************************************/
int LV_RefreshQRCodeArea(LPCOMMUNICATIONINFO pCommunicationInfo, LPCTSTR CodeStr);

/*********************************************************************************************
*	LV_SetBasicInfoEx						设置基本屏参  推荐使用Led Player软件进行设置
*
*	参数说明
*				pCommunicationInfo		通讯参数，赋值方式见COMMUNICATIONINFO结构体注示
*				ColorType				屏的颜色 1.单色  2.双基色  3.三基色   注：C卡全彩参数为3      X系列卡参数固定为 4
*				GrayLevel				灰度等级  赋值  1-5对应的灰度等级分别为 无,4,8,16,32  注：目前C系列的卡才支持，其它型号（T,A,U,XC,W,E,X）参数必须为0 
*				LedWidth				屏的宽度点数
*				LedHeight				屏的高度点数
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_SetBasicInfoEx(LPCOMMUNICATIONINFO pCommunicationInfo,int ColorType,int GrayLevel,int LedWidth,int LedHeight);
/*********************************************************************************************
*	LV_GetError								获取错误信息（只支持中文）
*
*	参数说明
*				nErrCode					函数执行返回的错误代码
*				nMaxCount					pErrStr字符串缓冲区的大小（为字符的个数，非字节数）
*				pErrStr						待获取错误信息的字符串地址
********************************************************************************************/
void LV_GetError(int nErrCode,int nMaxCount,OUT LPTSTR pErrStr);
/*********************************************************************************************
*	LV_PowerOnOff						设置屏开关
*
*	参数说明
*				pCommunicationInfo	通讯参数，赋值方式见LPCOMMUNICATIONINFO结构体注示
*				OnOff                   屏开关  0.开屏 1.关屏 2.重启 
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_PowerOnOff(LPCOMMUNICATIONINFO pCommunicationInfo, int OnOff);

/*********************************************************************************************
*	LV_AdjustTime						校时
*
*	参数说明
*				pCommunicationInfo	通讯参数，赋值方式见LPCOMMUNICATIONINFO结构体注示
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_AdjustTime(LPCOMMUNICATIONINFO pCommunicationInfo);

/*********************************************************************************************
*	LV_TestOnline						测试是否通讯正常
*
*	参数说明
*				pCommunicationInfo	通讯参数，赋值方式见LPCOMMUNICATIONINFO结构体注示
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息
********************************************************************************************/
int LV_TestOnline(LPCOMMUNICATIONINFO pCommunicationInfo);

/*********************************************************************************************
*	LV_LedInitServer			启动监听服务端口
*	
*	参数说明
*				port			监听的端口
*	返回值
*				0						成功
*				非0						失败，调用LV_GetError来获取错误信息	
********************************************************************************************/
int LV_InitServer(int port);

/*********************************************************************************************
*	LV_LedShudownServer			断开控制卡心跳包服务
********************************************************************************************/
int LV_ShudownServer();

/*********************************************************************************************
*	LV_RegisterLedServerCallback			注册回调函数
*	
*	参数说明
*				ledServerCallback					回调函数

********************************************************************************************/
void LV_RegisterServerCallback(SERVERINFOCALLBACK ledServerCallback);


/*********************************************************************************************
*	SetLedServerInfo		设置LED屏的服务器信息
*	
*	参数说明
*				pCommunicationInfo	通讯参数，赋值方式见LPCOMMUNICATIONINFO结构体注示
*				IsClient				1为启动客户端模式 0为不启动(当前带有广域网功能的卡默认为启动)
*				ServerIp				服务器IP
*				ServerPort				端口
*				HeartBeatTime			LED心跳包时间(最小20秒)
*	返回值
*				0						成功
*				非0						失败
********************************************************************************************/

int	 LV_SetServerInfo(LPCOMMUNICATIONINFO pCommunicationInfo,int isClient,LPCTSTR pServerIp,int nServerPort,int nHeartbeatTime);

	/*********************************************************************************************
	 *	_LV_SetRelay		设置LED屏的继电器器信息
	 *	
	 *	参数说明
	 *				pCommunicationInfo	通讯参数，赋值方式见LPCOMMUNICATIONINFO结构体注示
	 *				type				控制模式 0:手动模式  1:定时模式
	 *				manualSwitch		手动模式时的开关值 0:关  1:开
	 *				flag1				定时模式的第一组时间标志 0:无效 1:有效
	 *				startTime1			定时模式的第一组起始时间,格式如:"12:00:00"
	 *				endTime1			定时模式的第一组结束时间,格式如:"13:59:59"
	 *				flag2				定时模式的第二组时间标志 0:无效 1:有效
	 *				startTime2			定时模式的第二组起始时间,格式如:"14:00:00"
	 *				endTime2			定时模式的第二组结束时间,格式如:"15:59:59"
	 *				flag3				定时模式的第三组时间标志 0:无效 1:有效
	 *				startTime3			定时模式的第三组起始时间,格式如:"16:00:00"
	 *				endTime3			定时模式的第三组结束时间,格式如:"17:59:59"
	 *	返回值
	 *				0						成功
	 *				非0						失败
	 ********************************************************************************************/
int	 LV_SetRelay(LPCOMMUNICATIONINFO pCommunicationInfo,int type,int nManualSwitch
                                      , int flag1, LPCTSTR startTime1, LPCTSTR endTime1
                                      , int flag2,LPCTSTR startTime2,LPCTSTR endTime2
                                      , int flag3,LPCTSTR startTime3,LPCTSTR endTime3);
                                      
                                      
   /*********************************************************************************************
	 *	LV_SetRelay		设置LED屏的通讯数据
	 *	
	 *	参数说明
	 *				pCommunicationInfo	通讯参数，赋值方式见LPCOMMUNICATIONINFO结构体注示
	 *				pLedCommunicationParameter			修改的参数值

	 *	返回值
	 *				0						成功
	 *				非0						失败
	 ********************************************************************************************/
int    LV_SetLedCommunicationParameter(LPCOMMUNICATIONINFO pCommunicationInfo,LPLEDCOMMUNICATIONPARAMETER pLedCommunicationParameter);



}
#endif

