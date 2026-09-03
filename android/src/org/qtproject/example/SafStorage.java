package org.qtproject.example;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.os.StatFs;
import android.provider.DocumentsContract;

import java.io.OutputStream;
import java.lang.reflect.Method;

/**
 * Android SAF(存储访问框架)辅助类。
 *
 * 作用：
 *   1. 持久化用户通过系统“文件夹选择器”选中的目录授权
 *      （takePersistableUriPermission），保证 App 重启后仍可直接写入；
 *   2. 将 content:// 树 URI 解析为可读路径用于界面显示；
 *   3. 向所选目录内创建/覆盖并写入导出文件；
 *   4. 返回目录所在存储卷的总空间/可用空间，供导出前空间检测。
 *
 * 说明：本类由 C++(QJniObject) 静态调用，方法均为 public static。
 */
public class SafStorage
{
    private static Context sContext; // 缓存，避免每次反射

    /**
     * 获取 Qt 应用上下文。
     * QtNative.getContext()/activity() 为包级私有方法，外部包无法直接调用，
     * 故经反射访问（Qt 的 jar 随应用打包，非系统隐藏 API，可正常 setAccessible）。
     */
    private static Context context()
    {
        Context c = sContext;
        if (c != null)
            return c;
        try {
            Class<?> clazz = Class.forName("org.qtproject.qt.android.QtNative");
            Method getter = clazz.getDeclaredMethod("getContext");
            getter.setAccessible(true);
            Object obj = getter.invoke(null);
            if (obj instanceof Context)
                c = (Context) obj;
        } catch (Throwable t) {
            c = null; // 获取失败由调用方处理（各方法均已判空）
        }
        sContext = c;
        return c;
    }

    /** 持久化目录授权，保证应用重启后仍可向该目录写入。 */
    public static void persistPermission(String treeUri)
    {
        try {
            Context c = context();
            if (c == null)
                return;
            Uri tree = Uri.parse(treeUri);
            c.getContentResolver().takePersistableUriPermission(
                tree,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (Exception e) {
            // 部分系统/目录无法持久化时静默处理（本次会话内仍可写入）
        }
    }

    /**
     * 将 content:// 树 URI 解析为可读的展示路径，例如：
     *   "primary:Download/BLE_SAR" -> "内部存储/Download/BLE_SAR"
     * 失败时返回空串。
     */
    public static String displayPath(String treeUri)
    {
        try {
            Uri tree = Uri.parse(treeUri);
            String docId = DocumentsContract.getTreeDocumentId(tree);
            if (docId == null)
                return "";
            String pretty = docId;
            if (pretty.startsWith("primary:"))
                pretty = "内部存储/" + pretty.substring("primary:".length());
            return pretty;
        } catch (Exception e) {
            return "";
        }
    }

    /**
     * 向所选 SAF 目录内创建/覆盖文件并写入数据。
     * 成功返回文件的 content:// URI；失败返回 "ERR:<原因>"。
     */
    public static String saveFile(String treeUri, String fileName, byte[] data)
    {
        try {
            Context c = context();
            if (c == null)
                return "ERR:应用上下文不可用";
            ContentResolver resolver = c.getContentResolver();
            Uri tree = Uri.parse(treeUri);
            String treeDocId = DocumentsContract.getTreeDocumentId(tree);
            Uri treeDocUri = DocumentsContract.buildDocumentUriUsingTree(
                tree, treeDocId);

            // 同名文件已存在时先删除，保证“覆盖导出”而不是生成副本
            Uri existing = findChild(resolver, tree, fileName);
            if (existing != null)
                DocumentsContract.deleteDocument(resolver, existing);

            Uri fileUri = DocumentsContract.createDocument(
                resolver, treeDocUri, "text/plain", fileName);
            if (fileUri == null)
                return "ERR:创建文件失败，请确认所选目录可写";
            OutputStream os = resolver.openOutputStream(fileUri, "w");
            if (os == null)
                return "ERR:无法打开文件输出流";
            try {
                os.write(data);
                os.flush();
            } finally {
                os.close();
            }
            return fileUri.toString();
        } catch (Exception e) {
            return "ERR:" + (e.getMessage() != null
                    ? e.getMessage() : e.getClass().getSimpleName());
        }
    }

    /** 在树目录内按显示名查找已有文档，找不到返回 null。 */
    private static Uri findChild(ContentResolver resolver, Uri tree,
                                 String displayName)
    {
        try {
            String treeDocId = DocumentsContract.getTreeDocumentId(tree);
            Uri childrenUri = DocumentsContract
                .buildChildDocumentsUriUsingTree(tree, treeDocId);
            Cursor cur = resolver.query(
                childrenUri,
                new String[] {
                    DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                    DocumentsContract.Document.COLUMN_DISPLAY_NAME
                },
                null, null, null);
            if (cur != null) {
                try {
                    while (cur.moveToNext()) {
                        String name = cur.getString(1);
                        if (name != null && name.equals(displayName)) {
                            String id = cur.getString(0);
                            return DocumentsContract
                                .buildDocumentUriUsingTree(tree, id);
                        }
                    }
                } finally {
                    cur.close();
                }
            }
        } catch (Exception e) {
            // 忽略查询失败，交由上层 createDocument 处理
        }
        return null;
    }

    /**
     * 返回所选目录所在存储卷的总空间/可用空间。
     * 返回格式 "totalBytes;freeBytes"；失败返回 "-1;-1"。
     */
    public static String storageBytes(String treeUri)
    {
        try {
            String volumePath = null;
            if (treeUri != null && treeUri.startsWith("content://")) {
                try {
                    String docId = DocumentsContract
                        .getTreeDocumentId(Uri.parse(treeUri));
                    // 内部共享存储：primary:xxx -> /storage/emulated/0
                    if (docId != null && docId.startsWith("primary:"))
                        volumePath = Environment.getExternalStorageDirectory()
                            .getAbsolutePath();
                } catch (Exception ignored) {
                }
            }
            if (volumePath == null)
                volumePath = Environment.getExternalStorageDirectory()
                    .getAbsolutePath();
            StatFs stat = new StatFs(volumePath);
            return stat.getTotalBytes() + ";" + stat.getAvailableBytes();
        } catch (Exception e) {
            return "-1;-1";
        }
    }
}
