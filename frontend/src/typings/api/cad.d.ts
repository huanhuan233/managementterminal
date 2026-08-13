declare namespace Api {
  namespace Cad {
    type ParseStatusValue = 'uploaded' | 'queued' | 'processing' | 'completed' | 'failed' | 'deleted';

    interface UploadResponse {
      model_id: string;
      revision_id: string;
      status: ParseStatusValue;
    }

    interface ParseStatus {
      status: ParseStatusValue;
      progress: number;
      status_message: string | null;
      error_code: string | null;
      error_message: string | null;
    }

    interface ModelSummary {
      id: string;
      name: string;
      current_revision_id: string | null;
      status: ParseStatusValue | null;
      progress: number | null;
      face_count?: number;
      edge_count?: number;
      vertex_count?: number;
      created_at?: string;
    }

    interface PagedResult<T> {
      items: T[];
      total: number;
      page: number;
      page_size: number;
    }

    interface TreeNode {
      id: string;
      parent_entity_id: string | null;
      entity_type: string;
      label: string;
      source_ref: string | null;
      geometry_type: string | null;
      children: TreeNode[];
    }

    interface Entity {
      id: string;
      revision_id: string;
      parent_entity_id: string | null;
      entity_type: string;
      source_ref: string | null;
      source_index: number | null;
      name: string | null;
      label: string | null;
      tree_path: string;
      sort_order: number;
      geometry_type: string | null;
      area: number | null;
      volume: number | null;
      length: number | null;
      center: unknown;
      bounding_box: Record<string, unknown> | null;
      placement: Record<string, unknown> | null;
      geometry: Record<string, unknown>;
      metadata: Record<string, unknown>;
    }

    interface Mesh {
      id: string;
      revision_id: string;
      entity_id: string;
      mesh_type: 'face';
      positions: number[][];
      indices: number[][];
      normals: number[][] | null;
      color: unknown;
      linear_deflection: number;
      angular_deflection: number | null;
      vertex_count: number;
      triangle_count: number;
    }

    interface FaceTopology {
      edges: Entity[];
      adjacent_faces: Entity[];
      vertices: Entity[];
      faces: Entity[];
    }

    interface EdgeTopology {
      edges: Entity[];
      adjacent_faces: Entity[];
      vertices: Entity[];
      faces: Entity[];
    }

    interface Measurement {
      id: string;
      revision_id: string;
      scope_entity_id: string;
      feature_id: string | null;
      measurement_type: string;
      raw_value: Record<string, unknown>;
      normalized_value: Record<string, unknown>;
      unit: string | null;
      source_entity_ids: string[];
      method: string;
      confidence: number;
      algorithm_version: string;
      metadata: Record<string, unknown>;
      created_at: string;
    }

    interface FeatureCandidate {
      id: string;
      revision_id: string;
      scope_entity_id: string;
      feature_type: string;
      source_entity_ids: string[];
      parameters: Record<string, unknown>;
      axis: unknown;
      center: unknown;
      confidence: number;
      algorithm: string;
      algorithm_version: string;
      status: 'candidate' | 'confirmed' | 'rejected';
      metadata: Record<string, unknown>;
      created_at: string;
      updated_at: string;
    }

    interface PatternEvidence {
      center: number[];
      axis: number[];
      pitch_circle_diameter?: number;
    }
  }

  namespace CadSpec {
    type LayoutStatusValue =
      | 'created'
      | 'preprocessing_image'
      | 'detecting_layout'
      | 'cropping_regions'
      | 'layout_ready'
      | 'needs_manual_layout'
      | 'failed'
      | 'extracting_product_info'
      | 'extracting_table'
      | 'extracting_symbols'
      | 'selecting_target_row'
      | 'validating_result'
      | 'review_ready';

    interface Task {
      task_id: string;
      revision_id: string;
      status: LayoutStatusValue;
    }

    interface TaskSummary {
      task_id: string;
      revision_id: string;
      status: LayoutStatusValue;
      progress?: number | null;
      status_message?: string | null;
      file_name?: string | null;
      created_at?: string | null;
    }

    interface LayoutStartResponse {
      task_id: string;
      status: LayoutStatusValue;
    }

    interface LayoutStatus {
      task_id: string;
      status: LayoutStatusValue;
      progress?: number | null;
      status_message?: string | null;
      error_code?: string | null;
      error_message?: string | null;
    }

    interface ExtractionStatus {
      task_id: string;
      status: LayoutStatusValue;
      progress?: number | null;
      status_message?: string | null;
      error_code?: string | null;
      error_message?: string | null;
    }

    interface Region {
      id: string;
      region_type: string;
      provider: string;
      provider_region_type: string | null;
      bbox_normalized: number[];
      bbox_pixels: number[];
      padded_bbox_pixels: number[];
      confidence: number | null;
      sort_order: number;
      crop_file_name: string | null;
      crop_sha256: string | null;
      crop_width: number | null;
      crop_height: number | null;
      metadata: Record<string, unknown>;
    }

    interface ExtractResponse {
      task_id: string;
      status: LayoutStatusValue;
    }

    interface TargetRowResult {
      requested_code: string | null;
      requested_dn: number | null;
      matched_code: string | null;
      matched_dn: number | null;
      selected_row: Record<string, unknown>;
      row_bbox_local: number[] | null;
      selection_confidence: number | null;
      warnings: string[];
      inferred_from_filename: boolean;
      needs_review: boolean;
    }

    interface Fact {
      id: string | null;
      fact_key: string;
      fact_type: string;
      symbol: string | null;
      label: string | null;
      operator: string;
      raw_value: unknown;
      normalized_value: unknown;
      value_type: string | null;
      unit: string | null;
      source_region_id: string | null;
      source_bbox_original: number[] | null;
      source_bbox_normalized: number[] | null;
      source_bbox_precision: 'cell' | 'row' | 'region' | string | null;
      confidence: number | null;
      needs_review: boolean;
      metadata: Record<string, unknown>;
    }

    interface ExtractionResult {
      task_id: string;
      source_id: string;
      product_info: Record<string, unknown>;
      table: Record<string, unknown>;
      symbols: Record<string, unknown>;
      target_row: TargetRowResult;
      facts: Fact[];
      model_name: string;
      prompt_version: string;
      warnings: string[];
    }
  }

  namespace ComponentBuild {
    type NodeType =
      | 'root'
      | 'library'
      | 'family'
      | 'type'
      | 'subtype'
      | 'component'
      | 'build'
      | 'folder'
      | 'reference_step'
      | 'drawing'
      | 'component_spec'
      | 'fusion'
      | 'yaml'
      | 'future';

    type RawNodeType = NodeType | 'data_fusion' | 'component_spec' | 'publish_validation';

    type RetryRole = 'reference_step' | 'drawing';

    interface Target {
      revision_id?: string;
      task_id?: string;
    }

    interface TreeNode {
      id: string;
      label: string;
      label_en?: string | null;
      node_type: NodeType;
      status: string;
      progress: number | null;
      disabled: boolean;
      build_id: string | null;
      library_code?: string | null;
      category_code?: string | null;
      part_type_code?: string | null;
      component_id?: string | null;
      component_name?: string | null;
      target: Target | null;
      status_label?: string | null;
      status_message?: string | null;
      error_code?: string | null;
      error_message?: string | null;
      source_format?: 'STEP' | 'CATPART' | 'CATPRODUCT' | null;
      processing_route?: 'step_cad_parse' | 'catia_feature_center' | null;
      current_stage?: string | null;
      children: TreeNode[];
    }

    interface RawTreeNode {
      id?: string;
      name?: string;
      label?: string;
      label_en?: string | null;
      node_type?: RawNodeType | string;
      status?: string;
      progress?: number | null;
      disabled?: boolean;
      build_id?: string | null;
      library_code?: string | null;
      category_code?: string | null;
      part_type_code?: string | null;
      component_id?: string | null;
      component_name?: string | null;
      target?: Target | null;
      status_label?: string | null;
      status_message?: string | null;
      error_code?: string | null;
      error_message?: string | null;
      source_format?: 'STEP' | 'CATPART' | 'CATPRODUCT' | null;
      processing_route?: 'step_cad_parse' | 'catia_feature_center' | null;
      current_stage?: string | null;
      children?: RawTreeNode[];
    }

    interface SourceStatus {
      id: string | null;
      status: string;
      progress?: number | null;
      status_message?: string | null;
      error_code?: string | null;
      error_message?: string | null;
    }

    interface BuildDetail {
      id: string;
      catalog_node_id: string | null;
      catalog_path: string | null;
      component_id: string;
      component_name: string;
      component_type: string;
      component_subtype: string | null;
      family: string | null;
      standard_number: string | null;
      version: string;
      default_dn: number | null;
      default_pn: number | null;
      cad_model_id: string | null;
      cad_revision_id: string | null;
      drawing_task_id: string | null;
      status: string;
      status_message: string | null;
      error_code: string | null;
      error_message: string | null;
      task_id?: string | null;
      source_format?: 'STEP' | 'CATPART' | 'CATPRODUCT' | null;
      processing_route?: 'step_cad_parse' | 'catia_feature_center' | null;
      current_stage?: string | null;
      progress?: number | null;
      created_at: string;
      updated_at: string;
    }

    interface CatalogPart {
      catalog_node_id: string;
      part_type_code: string;
      label: string;
      label_en: string;
      id_prefix: string;
      sort_order: number;
    }

    interface CatalogCategory {
      catalog_node_id: string;
      category_code: string;
      label: string;
      label_en: string;
      sort_order: number;
      parts: CatalogPart[];
    }

    interface CatalogLibrary {
      catalog_node_id: string;
      library_code: string;
      label: string;
      label_en: string;
      sort_order: number;
      categories: CatalogCategory[];
    }

    interface CatalogResponse {
      libraries: CatalogLibrary[];
      categories: CatalogCategory[];
    }

    interface BuildStatus {
      build_id: string;
      status: string;
      status_message: string | null;
      error_code: string | null;
      error_message: string | null;
      sources: {
        reference_step: SourceStatus;
        drawing: SourceStatus;
      };
    }

    interface ViewerContract {
      part_id: string;
      task_id: string;
      status: string;
      current_stage: string | null;
      progress?: number | null;
      source_format: 'STEP' | 'CATPART' | 'CATPRODUCT';
      processing_route: 'step_cad_parse' | 'catia_feature_center';
      summary: {
        model_name: string;
        source_file_name: string | null;
        part_number: string;
        part_name: string;
        version: string;
        material: string;
        solid_count: number;
        native_feature_count: number;
        recognized_feature_count: number;
        feature_face_mapping_available: boolean;
      };
      bom: {
        assembly_mode: 'none' | 'single_part' | 'assembly';
        default_visible: boolean;
        part_count: number;
        nodes: ViewerBomNode[];
      };
      viewer_geometry?: {
        displayable: boolean;
        primitive_count: number;
        triangle_count: number;
        solid_count: number;
        empty_reason: string | null;
      };
      worker: {
        mode: string;
        worker_job_id?: string;
        status?: string;
        stage?: string;
      };
      viewer_asset: {
        glb_url: string;
        scene_manifest_url: string;
        face_mesh_map_url: string;
        feature_mesh_map_url: string;
        selection_index_url?: string | null;
      } | null;
      feature_center: {
        available: boolean;
        canonical_features_url?: string | null;
        feature_geometry_links_url?: string | null;
        measurements_url?: string | null;
        topology_faces_url?: string | null;
        topology_edges_url?: string | null;
      };
      native_semantics?: {
        available: boolean;
        features_url?: string | null;
        native_features_url?: string | null;
        topology_bodies_url?: string | null;
        topology_cells_url?: string | null;
        topology_wires_url?: string | null;
        topology_coedges_url?: string | null;
        mesh_face_map_url?: string | null;
        mesh_triangles_url?: string | null;
        feature_results_url?: string | null;
        feature_result_cells_url?: string | null;
        feature_topology_links_url?: string | null;
        product_references_url?: string | null;
        product_instances_url?: string | null;
        native_tree_nodes_url?: string | null;
        node_properties_url?: string | null;
        native_tree_diagnostics_url?: string | null;
        capabilities_url?: string | null;
      };
      error_code: string | null;
      error_message: string | null;
    }

    interface ViewerBomNode {
      node_id: string;
      parent_id: string;
      name: string;
      part_number: string;
      instance_name: string;
      version: string;
      material: string;
      node_type: 'assembly' | 'subassembly' | 'part' | 'body' | 'solid' | 'root' | 'imported_object';
      quantity: number;
      source_format: 'STEP' | 'CATPART' | 'CATPRODUCT';
      level: number;
      transform: Record<string, unknown> | null;
      mesh_primitive_ids: string[];
      descendant_mesh_primitive_ids?: string[];
      entity_ids: string[];
      descendant_entity_ids?: string[];
      solid_count: number;
      volume: number | null;
      bounding_box: Record<string, unknown> | null;
      assembly_path: string;
      constraint_status: string;
      constraint_count: number | null;
      children: ViewerBomNode[];
    }

    interface CreatePayload {
      category_code: string;
      part_type_code: string;
      component_name: string;
      standard_number?: string;
      version?: string;
      step_file?: File;
      source_file?: File;
      drawing_file?: File;
    }

    interface UpdatePayload extends CreatePayload {
      build_id: string;
    }

    type ComponentSpecFieldKind =
      | 'object'
      | 'object_array'
      | 'scalar_array'
      | 'text'
      | 'number'
      | 'boolean'
      | 'null'
      | 'generic';

    interface ComponentSpecField {
      key: string;
      path: string;
      label: string;
      required: boolean;
      read_only: boolean;
      source: string;
      comment: string;
      kind: ComponentSpecFieldKind;
      repeatable?: boolean;
      value_type?: 'text' | 'number' | 'boolean';
      fixed_value?: unknown;
      children?: ComponentSpecField[];
      item?: {
        kind: 'object';
        children: ComponentSpecField[];
      };
    }

    interface ComponentSpecSection {
      key: string;
      label: string;
      description: string;
      fields: ComponentSpecField[];
    }

    interface ComponentSpecDocument {
      build_id: string;
      schema: {
        schema_version: string;
        sections: ComponentSpecSection[];
      };
      data: Record<string, any>;
      yaml: string | null;
      source_filename: string | null;
      saved: boolean;
      updated_at: string | null;
    }

    interface ComponentSpecSavePayload {
      data: Record<string, any>;
      yaml: string;
      source_filename: string | null;
    }

    interface ComponentSpecPreviewPayload {
      data: Record<string, any>;
      yaml?: string;
      source_filename?: string | null;
    }

    interface FusionField {
      path: string;
      value: unknown;
      source: 'build' | 'drawing' | 'step' | 'derived';
      confidence: number;
      decision: 'filled' | 'preserved' | 'conflict';
      needs_review: boolean;
    }

    interface FusionResponse {
      build_id: string;
      status: 'completed';
      summary: {
        filled: number;
        preserved: number;
        conflicts: number;
        needs_review: number;
      };
      fields: FusionField[];
      warnings: string[];
      component_spec: Record<string, any>;
    }
  }
}
