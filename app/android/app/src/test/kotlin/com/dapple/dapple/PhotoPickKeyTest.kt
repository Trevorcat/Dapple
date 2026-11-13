package com.dapple.dapple

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PhotoPickKeyTest {
    @Test fun photoPickerItemIsTheScansMediaStoreId() {
        assertEquals("ms:1000012", PhotoPickPlugin.keyFor("content://media/picker/0/com.android.providers.media.photopicker/media/1000012", "a.jpg", 5))
    }

    @Test fun mediaDocumentIsTheScansMediaStoreId() {
        assertEquals("ms:42", PhotoPickPlugin.keyFor("content://com.android.providers.media.documents/document/image:42", "a.dng", 5))
    }

    @Test fun otherProvidersFallBackToNameAndSize() {
        assertEquals("imp:IMG_1.DNG|900", PhotoPickPlugin.keyFor("content://com.android.externalstorage.documents/document/primary:DCIM/IMG_1.DNG", "IMG_1.DNG", 900))
    }

    @Test fun onlyDevelopableTypes() {
        assertTrue(PhotoPickPlugin.supported("A.ARW"))
        assertTrue(PhotoPickPlugin.supported("b.jpeg"))
        assertFalse(PhotoPickPlugin.supported("c.png"))
        assertFalse(PhotoPickPlugin.supported("noext"))
    }
}
