;;; emacsos-storage.el --- System file saving policy -*- lexical-binding: t; -*-

;; Copyright (C) 2026 emacsOS contributors
;; SPDX-License-Identifier: GPL-3.0-or-later

;;; Commentary:

;; Loaded explicitly by the controlled system startup, never from DATA.
;; Browser retention remains a separate checkpoint after guest file sync.

;;; Code:

(setq write-region-inhibit-fsync nil)

(provide 'emacsos-storage)
;;; emacsos-storage.el ends here
